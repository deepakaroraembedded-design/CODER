const parser = require(require('path').resolve(process.argv[2]));
const aggregate = require(require('path').resolve(process.argv[3]));
const parse = parser.parseRecord || parser.default.parseRecord;
const sum = aggregate.aggregate || aggregate.default.aggregate;

let tests = 0;
let passed = 0;
function check(condition, name) {
  tests++;
  if (condition) {
    passed++;
  } else {
    console.error(`FAILED: ${name}`);
  }
}

const parsed = parse('  alpha=7 ');
check(parsed && parsed.key === 'alpha' && parsed.value === 7, 'parses trimmed record');
check(parse('broken') === null, 'rejects malformed input');
check(parse('=3') === null, 'rejects empty key');

const totals = sum(['alpha=7', 'beta=2', 'alpha=5', 'ignored', 'beta=3', '=1']);
check(totals.alpha === 12 && totals.beta === 5, 'aggregates valid lines');
check(Object.keys(totals).length === 2, 'ignores malformed lines');
console.log(JSON.stringify({ tests, passed }));
process.exit(passed === tests ? 0 : 1);
