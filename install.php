<?php

$options = implode(" ", array_splice($argv, 1));

// build xapian
//system("export CXXFLAGS=\"-fPIC\" && cd xapian-core && ./configure && make");

// compile
$I = array(
	'-I/usr/include/pgsql/server',
	'-Ixapian-core/include',
	'-Ixapian-core/common',
	'-Ixapian-core',
);

system("g++ -shared -o pgxapian.so -fPIC -luuid xapian.cpp xapian-core/.libs/libxapian.a " . join(' ', $I));

// clean object files
system("rm -f *.o");

// SQL types
system("psql $options -c \"create type xapian_document as ( document_id int, relevance int );\"");

// load SQL
echo "Installing functions...\n";
$pwd = trim(`pwd`);
$lines = explode("\n", file_get_contents("xapian.cpp"));
foreach($lines as $line) {
	if(substr($line, 0, 3) != '//@')
		continue;
	
	$words = explode(' ', substr($line, 0, strpos($line, '(')));
	$returns = implode(' ', array_slice($words, 1, count($words) - 2));
	$function = trim(implode(' ', array_slice($words, count($words) - 1))) . substr($line, strpos($line, '('));
	$bare = trim(implode(' ', array_slice($words, count($words) - 1, 1)));
	$sql = "CREATE OR REPLACE FUNCTION $function RETURNS $returns AS ".
	       "'$pwd/pgxapian.so', 'pg_$bare' LANGUAGE C STRICT";
	//echo "$sql\n";
	system("psql $options -c \"$sql\"");
}
echo "Done\n";

?>
