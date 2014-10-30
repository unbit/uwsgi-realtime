# this tool subscribes to a redis channel where each frame (audio and video)
# is sent with trackid and timestamp as prefix (uin8_t + uint32_t)
# Each packet is then reassembled in a webm cluster synchronizing audio and video
#

use Redis;

my $au_last_ts = 0;
my $vd_last_ts = 0;

my $cb = sub {
	my ($message, $channel) = @_;
	my ($channel, $ts) = unpack('CQ>', substr($message, 0, 9));
	my $payload = substr($message, 9);
	#print $channel.' '.$ts."\n";
	if ($channel == 0) {
		if ($au_last_ts > 0) {
			my $value = $ts - $au_last_ts;
			print '[-AAC]'.$ts."\n";
		}
		$au_last_ts = $ts;
		#print $au_last_ts."\n";
	}

	if ($channel == 2) {
                if ($vd_last_ts > 0) {
                        my $value = $ts - $vd_last_ts;
                        print '[H264]'.$ts."\n";
                }
                $vd_last_ts = $ts;
                #print $au_last_ts."\n";
        }
};

my $redis = Redis->new;
$redis->subscribe('foobar', $cb);

for(;;) {
	$redis->wait_for_messages(0);
}

