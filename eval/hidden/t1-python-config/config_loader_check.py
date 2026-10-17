#!/usr/bin/env python3
"""Hidden acceptance checks for the layered config loader (design 17.2).

Runs against the integration worktree: imports src/config_loader and checks
the contract from the plan's acceptance criteria. Every case is scored even
when the implementation raises, and the per-case counts are printed as a
final {"tests": N, "passed": M} line.
"""
import argparse
import json
import os
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--worktree", required=True)
    args = parser.parse_args()

    sys.path.insert(0, os.path.join(args.worktree, "src"))

    checks = 0
    passed = 0

    def case(run):
        nonlocal checks, passed
        checks += 1
        try:
            if run():
                passed += 1
        except Exception:  # noqa: BLE001 - any failure fails the case
            pass

    try:
        from config_loader import ConfigError, load_config
    except Exception as error:  # noqa: BLE001 - report any import failure
        print("cannot import config_loader: %s" % error, file=sys.stderr)
        print('{"tests": 1, "passed": 0}')
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "config.json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump({"b": 3, "extra": "kept"}, handle)
        bad = os.path.join(tmp, "bad.json")
        with open(bad, "w", encoding="utf-8") as handle:
            handle.write("{not json")
        not_object = os.path.join(tmp, "list.json")
        with open(not_object, "w", encoding="utf-8") as handle:
            json.dump([1, 2, 3], handle)

        # Defaults alone are returned.
        def defaults_only():
            config = load_config(defaults={"a": 1, "b": 2})
            return config["a"] == 1 and config["b"] == 2
        case(defaults_only)

        # The file layer overrides the defaults.
        def file_overrides():
            config = load_config(defaults={"a": 1, "b": 2}, path=path)
            return config["a"] == 1 and config["b"] == 3
        case(file_overrides)

        # Unknown file keys are kept in the result and reported, sorted.
        def unknown_keys():
            config = load_config(defaults={"a": 1, "b": 2}, path=path)
            return (config["extra"] == "kept"
                    and list(getattr(config, "unknown_keys", [])) == ["extra"])
        case(unknown_keys)

        # The environment layer wins over the file layer.
        def environment_wins():
            config = load_config(defaults={"a": 1, "b": 2}, path=path,
                                 env={"a": "10", "b": "env"})
            return config["a"] == "10" and config["b"] == "env"
        case(environment_wins)

        # No path: defaults plus environment.
        def no_path():
            config = load_config(defaults={"a": 1}, path=None, env={"a": "x"})
            return config["a"] == "x"
        case(no_path)

        # Malformed JSON raises ConfigError naming the path.
        def malformed_names_the_path():
            try:
                load_config(defaults={}, path=bad)
                return False
            except ConfigError as error:
                return bad in str(error)
            except Exception:  # noqa: BLE001 - a bare traceback fails the case
                return False
        case(malformed_names_the_path)

        # Valid JSON that is not an object is malformed too.
        def not_an_object():
            try:
                load_config(defaults={}, path=not_object)
                return False
            except ConfigError:
                return True
            except Exception:  # noqa: BLE001
                return False
        case(not_an_object)

    print('{"tests": %d, "passed": %d}' % (checks, passed))
    return 0 if passed == checks else 1


if __name__ == "__main__":
    raise SystemExit(main())
