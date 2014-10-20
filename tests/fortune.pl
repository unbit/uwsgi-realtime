use Redis;

my $redis = Redis->new;

for(;;) {
	$redis->publish($ARGV[rand @ARGV], '"'.`fortune`.'"');
	sleep(1);
}
