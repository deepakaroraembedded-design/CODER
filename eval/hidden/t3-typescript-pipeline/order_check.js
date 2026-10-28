const models = require(require('path').resolve(process.argv[2]));
const processor = require(require('path').resolve(process.argv[3]));
const { LineItem, Money } = models;
const processOrder = processor.process;

let tests = 0;
let passed = 0;
function check(condition, name) {
  tests++;
  if (condition) passed++;
  else console.error(`FAILED: ${name}`);
}

let result = processOrder([]);
check(result.ledger.itemCount() === 0 && result.total.cents === 0, 'empty order');
result = processOrder([
  new LineItem('A', 2, new Money(500)),
  new LineItem('B', 1, new Money(250)),
  new LineItem('A', 1, new Money(100)),
]);
check(result.ledger.itemCount() === 3, 'duplicate SKUs distinct');
check(result.total.cents === 1350, 'total');
let rejected = false;
try { new LineItem('X', 0, new Money(1)); } catch (error) { rejected = error instanceof TypeError; }
check(rejected, 'rejects zero quantity');
console.log(JSON.stringify({ tests, passed }));
process.exit(passed === tests ? 0 : 1);
