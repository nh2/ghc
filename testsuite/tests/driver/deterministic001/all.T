# Test for #4012, deterministic compilation

test('deterministic001',
     [ clean_cmd('$MAKE -s clean') ],
     run_command,
     ['$MAKE -s --no-print-directory deterministic001'])

