<?php

$options = implode(" ", array_splice($argv, 1));

system("psql $options -c \"select xapian_version();\"");

?>
