//post a task running in your .dll

// post a task running in your background.js
var bind  = process.binding('pt_c');
var obj = {param1: 123,param2: 456};
bind.jstask('globalFunction', JSON.stringify(obj), function(err, data){if(err) console.log("err"); else console.log(data);});