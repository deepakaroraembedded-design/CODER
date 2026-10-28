const words = require(require('path').resolve(process.argv[2]));
const count = words.countWords || words.default.countWords;

let tests = 0;
let passed = 0;
function check(condition, name) {
  tests++;
  if (condition) passed++;
  else console.error(`FAILED: ${name}`);
}

const counts = count('Hello, world! Hello TypeScript');
check(counts.hello === 2 && counts.world === 1 && counts.typescript === 1, 'counts words');
check(Object.keys(count('')).length === 0, 'empty input');
check(count('a-b a_b').a === 2 && count('a-b a_b').b === 1, 'punctuation separates');
console.log(JSON.stringify({ tests, passed }));
process.exit(passed === tests ? 0 : 1);
