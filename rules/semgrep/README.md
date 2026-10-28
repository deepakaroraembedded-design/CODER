# Company Semgrep rules

Starter `swt.*` rules for the non-C/C++ toolchain profiles. The profiles load
one file per language (`python.yaml`, `typescript.yaml`, `javascript.yaml`,
`java.yaml`, `go.yaml`) and treat findings as gate failures (`--error`).

These rules are intentionally small: they cover high-signal resource,
logging/secrets and injection hazards from the company guidelines. Add one rule
per mandatory guideline item and give it an immutable `swt.<language>.<rule>`
ID. Validate locally with:

```bash
semgrep --validate --config rules/semgrep/<language>.yaml
semgrep --config rules/semgrep/<language>.yaml --error <paths>
```

The `cf-swt-*` checks for C/C++ live in `cf-tidy`, not here (design §6.5).
