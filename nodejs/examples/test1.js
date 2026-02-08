console.log('Before require...');

const crun = require('../build/Release/crun_binding.node');

// console.log('After require - this should appear now!');
// console.log('Available functions:', Object.keys(crun));

const containers = crun.list();
console.log('Containers:', containers);