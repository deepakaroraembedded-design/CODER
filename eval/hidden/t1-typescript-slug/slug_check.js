const path = require('path');
const modulePath = process.argv[2];
const loaded = require(path.resolve(modulePath));
const slugify = loaded.slugify || loaded.default;

let tests = 0;
let passed = 0;
function check(actual, expected, name) {
  tests++;
  if (actual === expected) {
    passed++;
  } else {
    console.error(`FAILED: ${name}: ${JSON.stringify(actual)} !== ${JSON.stringify(expected)}`);
  }
}
check(slugify('  Hello World!  '), 'hello-world', 'basic words');
check(slugify('A/B__C'), 'a-b-c', 'punctuation runs');
check(slugify('---already---'), 'already', 'strips edge hyphens');
check(slugify(''), '', 'empty input');
console.log(JSON.stringify({ tests, passed }));
process.exit(passed === tests ? 0 : 1);
