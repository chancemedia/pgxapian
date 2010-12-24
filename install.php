<?php

$options = implode(" ", array_splice($argv, 1));

// build xapian
//system("cd xapian-core && ./configure && make");

// compile
$I = array(
	'-I/usr/include/pgsql/server',
	'-Ixapian-core/include',
	'-Ixapian-core'
);
$files = array(
	'xapian-core/api/version.cc',
	'xapian.cpp'
);
system("g++ -fno-exceptions -fPIC -c " . join(' ', $I) . " " . join(' ', $files));
system("ld -shared -o pgxapian.so *.o");

// clean object files
system("rm -f *.o");

// load SQL
echo "Installing functions...\n";
$pwd = trim(`pwd`);
$lines = explode("\n", file_get_contents("xapian.cpp"));
foreach($lines as $line) {
	if(substr($line, 0, 3) != '//@')
		continue;
	
	$returns = trim(substr($line, 3, strpos($line, ' ', 4) - 3));
	$function = trim(substr($line, strpos($line, ' ', 4)));
	$bare = substr($function, 0, strpos($function, '('));
	$sql = "CREATE OR REPLACE FUNCTION $function RETURNS $returns AS ".
	       "'$pwd/pgxapian.so', 'pg_$bare' LANGUAGE C STRICT";
	system("psql $options -c \"$sql\"");
}
echo "Done\n";

?>
