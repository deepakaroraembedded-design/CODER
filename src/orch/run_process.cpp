#include "cf/orch/run_process.hpp"

#include <algorithm>
#include <memory>
#include <system_error>
#include <utility>

#include <spdlog/spdlog.h>
#include <unistd.h>

#include "cf/orch/escalation.hpp"
#include "cf/orch/defect_export.hpp"
#include "cf/orch/github_flow.hpp"
#include "cf/orch/recovery.hpp"
#include "cf/rag/loader.hpp"
#include "cf/rag/retriever.hpp"
#include "cf/sandbox/docker.hpp"

namespace cf::orch {
namespace {

// The pipeline hands paths to git and to opencode, both of which may resolve
// them against a different working directory (the repository or the role
// worktree); anchor them to absolute paths once, here.
void anchor_path(std::filesystem::path& path) {
  if (path.empty()) {
    return;
  }
  std::error_code ec;
  const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
  if (!ec) {
    path = absolute;
  }
}

void anchor_binary(std::string& binary) {
  if (binary.empty() || binary.find('/') == std::string::npos) {
    return;  // a bare name resolves through PATH
  }
  std::filesystem::path path = binary;
  anchor_path(path);
  binary = path.string();
}

Result<rag::RetrieverOptions> retriever_options(const RunProcessConfig& config) {
  rag::RetrieverOptions options;
  if (config.rag_index.has_value()) {
    options.hnsw = *config.rag_index != "brute-force";
  }
  if (config.rag_bm25_weight.has_value()) {
    options.bm25_weight = *config.rag_bm25_weight;
  }
  if (config.rag_vector_weight.has_value()) {
    options.vector_weight = *config.rag_vector_weight;
  }
  if (config.rag_precedence.has_value()) {
    auto types = rag::doc_types_from_names(*config.rag_precedence);
    if (!types) {
      return tl::make_unexpected(types.error());
    }
    options.precedence = std::move(*types);
  }
  return options;
}

// The profile's agent commands (build system plus test framework), as the
// comma-separated list the role prompts show.
std::string agent_command_summary(const gates::Profile& profile,
                                  const std::string& build_system,
                                  const std::string& test_framework) {
  std::vector<std::string> commands;
  const auto build = profile.build_systems.find(build_system);
  if (build != profile.build_systems.end()) {
    commands = build->second.agent_commands;
  }
  const auto tests = profile.test_frameworks.find(test_framework);
  if (tests != profile.test_frameworks.end()) {
    for (const auto& step : tests->second.steps) {
      if (step.argv.empty()) {
        continue;
      }
      const std::string& program = step.argv.front();
      if (std::find(commands.begin(), commands.end(), program) == commands.end()) {
        commands.push_back(program);
      }
    }
  }
  std::string out;
  for (const auto& command : commands) {
    if (!out.empty()) {
      out += ", ";
    }
    out += command;
  }
  return out;
}

RunProcessConfig anchor_config(RunProcessConfig config) {
  anchor_path(config.workspace_root);
  anchor_binary(config.opencode_binary);
  anchor_binary(config.mcp_command);
  if (!config.docs_dir.empty()) {
    std::filesystem::path docs = config.docs_dir;
    anchor_path(docs);
    config.docs_dir = docs.string();
  }
  return config;
}

}  // namespace

RunProcess::RunProcess(store::RunStore& store, plan::Plan plan, RunProcessConfig config,
                       git::Repository& repository, process::ProcessExecutor& executor)
    : store_(store),
      plan_(std::move(plan)),
      config_(anchor_config(std::move(config))),
      repository_(repository),
      executor_(executor) {}

void RunProcess::set_session_driver_factory(SessionDriverFactory factory) {
  driver_factory_ = std::move(factory);
}

Result<RunState> RunProcess::execute(store::UnixTime now) {
  const std::filesystem::path run_dir =
      config_.workspace_root / "runs" / config_.run_id;
  std::error_code ec;
  std::filesystem::create_directories(run_dir, ec);

  // Code intelligence (design section 6.5): when a back end matches the
  // plan's language, the run keeps the symbol index at runs/<run>/
  // codeintel.db in step with the integration branch and the role MCP
  // servers read it through --codeintel-db.
  std::string codeintel_db;
  std::unique_ptr<CodeIntelSync> codeintel_sync;
  if (config_.codeintel_backends != nullptr) {
    auto backend =
        config_.codeintel_backends->for_language(plan_.target.language);
    if (backend) {
      auto sync = CodeIntelSync::open(
          **backend, repository_, run_dir / "codeintel.db",
          config_.workspace_root / "wt" / config_.run_id / "integration");
      if (!sync) {
        return tl::make_unexpected(sync.error());
      }
      codeintel_sync = std::move(*sync);
      codeintel_db = codeintel_sync->db_path().string();
    } else if (backend.error().code != codeintel::error_codes::kUnknownLanguage) {
      return tl::make_unexpected(backend.error());
    } else {
      spdlog::info("codeintel: no back end for language '{}', indexing off",
                   plan_.target.language);
    }
  }

  std::unique_ptr<OpencodeRoleFactory> production_factory;
  SessionDriverFactory driver_factory = driver_factory_;

  // RAG index for question answering (design section 6.6.1 layer 3).
  std::shared_ptr<rag::InMemoryRetriever> retriever;
  const std::filesystem::path designs_dir = config_.workspace_root / "designs";
  if (!config_.docs_dir.empty() || std::filesystem::is_directory(designs_dir)) {
    auto options = retriever_options(config_);
    if (!options) {
      return tl::make_unexpected(options.error());
    }
    retriever = std::make_shared<rag::InMemoryRetriever>(*options);
    // Company documents plus approved design packages from earlier runs
    // (design section 6.2 step 7); the applicability filter keeps only the
    // ones scoped to this run's product and language.
    for (const auto& directory :
         {std::filesystem::path(config_.docs_dir), designs_dir}) {
      std::error_code directory_ec;
      if (directory.empty() ||
          !std::filesystem::is_directory(directory, directory_ec)) {
        continue;
      }
      auto documents = rag::load_document_dir(directory);
      if (documents) {
        for (auto& document : *documents) {
          auto added = retriever->add_document(std::move(document));
          if (!added) {
            spdlog::warn("cannot index {}: {}", document.doc_id,
                         added.error().message);
          }
        }
      } else {
        spdlog::warn("cannot load documents from {} for question answering: {}",
                     directory.string(), documents.error().message);
      }
    }
  }

  if (!driver_factory) {
    OpencodeFactoryConfig factory_config;
    factory_config.run_id = config_.run_id;
    factory_config.run_dir = run_dir;
    factory_config.opencode_binary = config_.opencode_binary;
    factory_config.provider_base_url = config_.provider_base_url;
    factory_config.api_key_env = config_.api_key_env;
    factory_config.role_models = config_.role_models;
    factory_config.role_options = config_.role_options;
    factory_config.mcp_command = config_.mcp_command;
    factory_config.db_path = (run_dir / "run.db").string();
    factory_config.docs_dir = config_.docs_dir;
    // RAG applicability context and approved design packages (design
    // sections 6.2 step 7, 6.3).
    factory_config.product = plan_.product;
    factory_config.language = plan_.target.language;
    const std::filesystem::path designs = config_.workspace_root / "designs";
    std::error_code designs_ec;
    if (std::filesystem::is_directory(designs, designs_ec)) {
      factory_config.designs_dir = designs.string();
    }
    factory_config.patterns_dir = config_.patterns_dir;
    factory_config.rag_top_k = config_.rag_top_k;
    factory_config.rag_index = config_.rag_index;
    factory_config.rag_bm25_weight = config_.rag_bm25_weight;
    factory_config.rag_vector_weight = config_.rag_vector_weight;
    factory_config.rag_precedence = config_.rag_precedence;
    const auto build =
        config_.profile.build_systems.find(config_.profile_build_system);
    if (build != config_.profile.build_systems.end()) {
      factory_config.coder_commands = build->second.agent_commands;
      factory_config.qa_commands = build->second.agent_commands;
    }
    const auto tests =
        config_.profile.test_frameworks.find(config_.profile_test_framework);
    if (tests != config_.profile.test_frameworks.end()) {
      for (const auto& step : tests->second.steps) {
        if (!step.argv.empty()) {
          factory_config.qa_commands.push_back(step.argv.front());
        }
      }
    }
    factory_config.environment = config_.environment;
    factory_config.retriever = retriever.get();
    factory_config.ambiguities_file = run_dir / "ambiguities.jsonl";
    factory_config.codeintel_db = codeintel_db;
    factory_config.sandbox_image = config_.agent_sandbox_image;
    factory_config.sandbox_network = config_.agent_sandbox_network;
    factory_config.sandbox_binary = config_.agent_sandbox_binary;
    factory_config.docker_binary = config_.docker_binary;
    factory_config.mcp_binary_host = config_.sandbox_mcp_binary_host;
    factory_config.sandbox_allowed_hosts = config_.sandbox_allowed_hosts;
    production_factory =
        std::make_unique<OpencodeRoleFactory>(std::move(factory_config));
    OpencodeRoleFactory* raw = production_factory.get();
    driver_factory = [raw](const std::string& role, const std::string& worktree) {
      return raw->create(role, worktree);
    };
  }

  OpencodeRunnerOptions runner_options;
  runner_options.turn_timeout = config_.turn_timeout;
  runner_options.target_language = plan_.target.language;
  runner_options.target_standard = plan_.target.standard;
  runner_options.allowed_commands = agent_command_summary(
      config_.profile, config_.profile_build_system, config_.profile_test_framework);
  SessionAgentRunner agent_runner(driver_factory, run_dir, runner_options);

  DriverConfig driver_config;
  driver_config.run_id = config_.run_id;
  driver_config.workspace_root = config_.workspace_root;
  driver_config.integration_branch = config_.integration_branch;
  driver_config.base_branch = config_.base_branch;
  driver_config.profile = config_.profile;
  driver_config.profile_language = config_.profile_language;
  driver_config.profile_build_system = config_.profile_build_system;
  driver_config.profile_test_framework = config_.profile_test_framework;
  driver_config.step_timeout = config_.step_timeout;
  driver_config.sandbox_image = config_.sandbox_image;
  driver_config.full_review_threshold = config_.full_review_threshold;
  driver_config.gate_cpus = config_.gate_cpus;
  driver_config.gate_memory_mb = config_.gate_memory_mb;
  driver_config.codeintel = codeintel_sync.get();
  WorkspaceDriver driver(plan_, driver_config, repository_, executor_, agent_runner,
                         &store_);

  RunOrchestrator orchestrator(store_, config_.run_id, plan_, driver,
                               config_.orchestrator);

  const std::string holder = "run-process-" + std::to_string(::getpid());

  // GitHub flow: push per milestone, draft PR from integration (10.4.4).
  // The handlers double as reconcilers, so recovery gets them too.
  std::unique_ptr<github::Client> github_client;
  Dispatcher operation_dispatcher(store_, config_.run_id, holder);
  const bool github_enabled = config_.github_transport != nullptr &&
                              !config_.github_owner.empty() &&
                              !config_.github_repo.empty();
  if (github_enabled) {
    github::ClientOptions client_options;
    client_options.api_base = config_.github_api_base;
    client_options.token = config_.github_token;
    client_options.token_provider = config_.github_token_provider;
    github_client =
        std::make_unique<github::Client>(client_options, *config_.github_transport);
    GithubHandlerContext handler_context;
    handler_context.repository = &repository_;
    handler_context.client = github_client.get();
    handler_context.owner = config_.github_owner;
    handler_context.repo = config_.github_repo;
    handler_context.remote = config_.github_remote;
    operation_dispatcher.register_handler(
        std::make_unique<PushHandler>(handler_context));
    operation_dispatcher.register_handler(
        std::make_unique<PrCreateHandler>(handler_context));
    operation_dispatcher.register_handler(
        std::make_unique<PrUpdateHandler>(handler_context));
    operation_dispatcher.register_handler(
        std::make_unique<IssueCreateHandler>(handler_context));
    operation_dispatcher.register_handler(
        std::make_unique<IssueUpdateHandler>(handler_context));
    operation_dispatcher.register_handler(
        std::make_unique<CommentHandler>(handler_context));
  }

  StateMachine machine(store_, config_.run_id);
  RunState state = machine.current();
  if (state != RunState::Executing && state != RunState::SystemValidation) {
    return state;  // waiting on a person or already terminal
  }

  // Recovery runs automatically on start-up (design section 6.12.4): take
  // the run lease, release stale locks, reconcile operations left in flight
  // with the real handlers, and reset worktrees to their checkpoints before
  // any new effect is issued.
  Recovery recovery(
      store_,
      std::make_unique<GitWorktreeHook>(repository_, config_.workspace_root));
  RecoveryOptions recovery_options;
  recovery_options.holder = holder;
  recovery_options.lease_ttl = config_.lease_ttl;
  recovery_options.in_flight_grace = config_.in_flight_grace;
  if (!config_.agent_sandbox_image.empty() || !config_.sandbox_image.empty()) {
    // Remove containers left behind by the killed orchestrator before new
    // gate jobs or agent servers start (design section 6.12.4 step 2).
    const std::string docker_binary = config_.docker_binary;
    const std::string run_id = config_.run_id;
    recovery_options.kill_leftovers = [docker_binary, run_id]() {
      return sandbox::remove_run_containers(docker_binary, run_id);
    };
  }
  auto recovered =
      recovery.resume(config_.run_id, operation_dispatcher, recovery_options, now);
  if (!recovered) {
    return tl::make_unexpected(recovered.error());
  }
  now = recovered->reconciled_at;
  const uint64_t epoch = recovered->epoch;

  // With recovery complete the worktrees are settled: bring the symbol index
  // to the integration head before the first agent turn (design section 6.5).
  auto prepared = driver.prepare_codeintel();
  if (!prepared) {
    return tl::make_unexpected(prepared.error());
  }

  std::unique_ptr<GithubFlow> github_flow;
  std::unique_ptr<DefectExporter> defect_export;
  if (github_enabled) {
    GithubFlowConfig flow_config;
    flow_config.owner = config_.github_owner;
    flow_config.repo = config_.github_repo;
    flow_config.remote = config_.github_remote;
    flow_config.base_branch = config_.base_branch;
    flow_config.pre_push = config_.github_pre_push;
    flow_config.pr_draft_until_done = config_.github_pr_draft_until_done;
    flow_config.pr_labels = config_.github_pr_labels;
    flow_config.pr_request_codeowners =
        config_.github_pr_request_codeowners;
    flow_config.report_path =
        (std::filesystem::path("runs") / config_.run_id / "report.md").string();
    github_flow = std::make_unique<GithubFlow>(store_, operation_dispatcher,
                                               repository_, std::move(flow_config));
    if (config_.export_defects) {
      DefectExportConfig export_config;
      export_config.owner = config_.github_owner;
      export_config.repo = config_.github_repo;
      export_config.report_base =
          (config_.workspace_root / "runs" / config_.run_id).string();
      export_config.client = github_client.get();
      defect_export = std::make_unique<DefectExporter>(
          store_, operation_dispatcher, std::move(export_config));
    }
  }

  uint32_t validation_failures = 0;
  for (int step = 0; step < 100000; ++step) {
    auto next = orchestrator.step(now);
    if (!next) {
      return tl::make_unexpected(next.error());
    }
    const RunState previous = state;
    state = *next;

    // Sync the GitHub state after every step: newly Done milestones are
    // pushed, the draft PR is opened or updated, and Done marks it ready.
    if (github_flow) {
      auto synced = github_flow->sync(config_.run_id, plan_, state, epoch, now);
      if (!synced) {
        // A pre-push check is a local policy gate (design section 10.4.6):
        // refuse the push and escalate so a human decides, rather than
        // failing the process in a non-terminal state.
        if (synced.error().code == github_error_codes::kPrePushRejected &&
            (state == RunState::Executing ||
             state == RunState::SystemValidation)) {
          if (state == RunState::SystemValidation) {
            const auto reopened =
                machine.apply(RunEvent::SystemDefectsFiled, {}, now);
            if (!reopened) {
              return tl::make_unexpected(reopened.error());
            }
          }
          const auto applied = machine.apply(RunEvent::NoProgress, {}, now);
          if (!applied) {
            return tl::make_unexpected(applied.error());
          }
          if (!config_.orchestrator.escalation_dir.empty()) {
            (void)write_escalation_bundle(store_, config_.run_id,
                                          config_.orchestrator.escalation_dir,
                                          synced.error().message, now);
          }
          return *applied;
        }
        return tl::make_unexpected(synced.error());
      }
    }
    if (defect_export) {
      auto synced = defect_export->sync(config_.run_id, epoch, now);
      if (!synced) {
        return tl::make_unexpected(synced.error());
      }
    }

    if (state == RunState::Done || state == RunState::Escalated ||
        state == RunState::Aborted) {
      return state;
    }
    if (state == RunState::SystemValidation && previous == RunState::SystemValidation) {
      // The validation attempt did not complete (a stalled or failed agent
      // turn): retry a bounded number of times, then escalate instead of
      // stopping the run in a non-terminal state.
      if (++validation_failures > config_.max_system_validation_attempts) {
        const auto applied = machine.apply(RunEvent::NoProgress, {}, now);
        if (!applied) {
          return tl::make_unexpected(applied.error());
        }
        if (!config_.orchestrator.escalation_dir.empty()) {
          (void)write_escalation_bundle(store_, config_.run_id,
                                        config_.orchestrator.escalation_dir,
                                        "system validation failed repeatedly", now);
        }
        return *applied;
      }
      continue;
    }
    if (state == RunState::Executing && previous == RunState::SystemValidation) {
      if (++validation_failures > config_.max_system_validation_attempts) {
        // System validation keeps failing without any milestone reopening:
        // escalate instead of looping (design section 12).
        const auto applied = machine.apply(RunEvent::NoProgress, {}, now);
        if (!applied) {
          return tl::make_unexpected(applied.error());
        }
        if (!config_.orchestrator.escalation_dir.empty()) {
          (void)write_escalation_bundle(store_, config_.run_id,
                                        config_.orchestrator.escalation_dir,
                                        "system validation failed repeatedly", now);
        }
        return *applied;
      }
      continue;
    }
    if (state != RunState::Executing && state != RunState::SystemValidation) {
      return state;  // waiting on a person
    }
  }
  return fail("ORCH_STEP_LIMIT", "the run did not finish within the step limit");
}

}  // namespace cf::orch
