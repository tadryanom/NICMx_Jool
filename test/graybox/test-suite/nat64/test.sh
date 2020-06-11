#!/bin/sh


# Arguments:
# $1: List of the names of the test groups you want to run, separated by any
#     character.
#     Example: "udp64, tcp46, icmpe64"
#     If this argument is unspecified, the script will run all the tests.
#     The current groups are:
#     - udp64: IPv6->IPv4 UDP tests
#     - udp46: IPv4->IPv6 UDP tests
#     - tcp64: IPv6->IPv4 TCP tests
#     - icmpi64: IPv6->IPv4 ICMP ping tests
#     - icmpi46: IPv4->IPv6 ICMP ping tests
#     - icmpe64: IPv6->IPv4 ICMP error tests
#     - icmpe46: IPv4->IPv6 ICMP error tests
#     - misc: random tests we've designed later.
#     (Feel free to add new groups if you want.)


GRAYBOX=`dirname $0`/../../usr/graybox
# When there's no fragmentation, Jool is supposed to randomize the
# fragment ID (bytes 4 and 5) so we can't validate it.
# The ID's randomization cascades to the checksum. (Bytes 10 and
# 11.)
NOFRAG_IGNORE=4,5,10,11
NOFRAG_IGNORE_INNER=32,33,38,39

test_auto() {
	ip netns exec $1 $GRAYBOX expect add `dirname $0`/pktgen/receiver/$4-nofrag.pkt $5
	ip netns exec $2 $GRAYBOX send `dirname $0`/pktgen/sender/$3-nofrag.pkt
	sleep 0.1
	ip netns exec $1 $GRAYBOX expect flush
}

test64_auto() {
	test_auto client4ns client6ns $1 $2 $3
}

test46_auto() {
	test_auto client6ns client4ns $1 $2 $3
}

test_11() {
	ip netns exec $1 $GRAYBOX expect add `dirname $0`/manual/$4.pkt $5
	ip netns exec $2 $GRAYBOX send `dirname $0`/manual/$3.pkt
	sleep 0.1
	ip netns exec $1 $GRAYBOX expect flush
}

test64_11() {
	test_11 client4ns client6ns $1 $2 $3
}

test46_11() {
	test_11 client6ns client4ns $1 $2 $3
}

test66_11() {
	test_11 client6ns client6ns $1 $2 $3
}

test44_11() {
	test_11 client4ns client4ns $1 $2 $3
}


`dirname $0`/../wait.sh 64:ff9b::192.0.2.5
if [ $? -ne 0 ]; then
	exit 1
fi

echo "Testing! Please wait..."


# UDP, 6 -> 4
if [ -z "$1" -o "$1" = "udp64" ]; then
	test64_auto 6-udp-csumok-df 4-udp-csumok-df $NOFRAG_IGNORE
	test64_auto 6-udp-csumfail-df 4-udp-csumfail-df $NOFRAG_IGNORE
	test64_auto 6-udp-csumok-nodf 4-udp-csumok-nodf $NOFRAG_IGNORE
	test64_auto 6-udp-csumfail-nodf 4-udp-csumfail-nodf $NOFRAG_IGNORE
fi

# UDP, 4 -> 6
if [ -z "$1" -o "$1" = "udp46" ]; then
	test46_auto 4-udp-csumok-df 6-udp-csumok-df
	test46_auto 4-udp-csumfail-df 6-udp-csumfail-df
	test46_auto 4-udp-csumok-nodf 6-udp-csumok-nodf
	test46_auto 4-udp-csumfail-nodf 6-udp-csumfail-nodf
fi

# TCP
if [ -z "$1" -o "$1" = "tcp64" ]]; then
	test64_auto 6-tcp-csumok-df 4-tcp-csumok-df $NOFRAG_IGNORE
	test64_auto 6-tcp-csumfail-df 4-tcp-csumfail-df $NOFRAG_IGNORE
	test64_auto 6-tcp-csumok-nodf 4-tcp-csumok-nodf $NOFRAG_IGNORE
	test64_auto 6-tcp-csumfail-nodf 4-tcp-csumfail-nodf $NOFRAG_IGNORE
fi

# ICMP info, 6 -> 4
if [ -z "$1" -o "$1" = "icmpi64" ]; then
	test64_auto 6-icmp6info-csumok-df 4-icmp4info-csumok-df $NOFRAG_IGNORE
	test64_auto 6-icmp6info-csumfail-df 4-icmp4info-csumfail-df $NOFRAG_IGNORE
	test64_auto 6-icmp6info-csumok-nodf 4-icmp4info-csumok-nodf $NOFRAG_IGNORE
	test64_auto 6-icmp6info-csumfail-nodf 4-icmp4info-csumfail-nodf $NOFRAG_IGNORE
fi

# ICMP info, 4 -> 6
if [ -z "$1" -o "$1" = "icmpi46" ]; then
	test46_auto 4-icmp4info-csumok-df 6-icmp6info-csumok-df
	test46_auto 4-icmp4info-csumfail-df 6-icmp6info-csumfail-df
	test46_auto 4-icmp4info-csumok-nodf 6-icmp6info-csumok-nodf
	test46_auto 4-icmp4info-csumfail-nodf 6-icmp6info-csumfail-nodf
fi

# ICMP error, 6 -> 4
if [ -z "$1" -o "$1" = "icmpe64" ]; then
	test64_auto 6-icmp6err-csumok-df 4-icmp4err-csumok-df $NOFRAG_IGNORE,$NOFRAG_IGNORE_INNER
	test64_auto 6-icmp6err-csumok-nodf 4-icmp4err-csumok-nodf $NOFRAG_IGNORE,$NOFRAG_IGNORE_INNER
fi

# ICMP error, 4 -> 6
if [ -z "$1" -o "$1" = "icmpe46" ]; then
	test46_auto 4-icmp4err-csumok-df 6-icmp6err-csumok-df
	test46_auto 4-icmp4err-csumok-nodf 6-icmp6err-csumok-nodf
fi

# Miscellaneous tests
if [ -z "$1" -o "$1" = "misc" ]; then
	# Those zeroize-traffic-classes patch the 192 TOS Linux quirk.
	ip netns exec joolns jool g u zeroize-traffic-class true
	test66_11 issue132/test issue132/expected-on
	ip netns exec joolns jool g u source-icmpv6-errors-better off
	test66_11 issue132/test issue132/expected-off
	ip netns exec joolns jool g u source-icmpv6-errors-better on
	ip netns exec joolns jool g u zeroize-traffic-class false
fi

# ICMP errors (Generated by Jool rather than being translated)
if [ -z "$1" -o "$1" = "errors" ]; then
	# Try sending a ping for which there is no state.
	# (1 = TOS, 4,5 = ID, 6 = DF, 10,11 = Checksum)
	test44_11 icmp-error/bibless-test icmp-error/bibless-expected 1,4,5,10,11

	# Try sending large packets.
	sudo ip netns exec joolns ip link set to_client_v4 mtu 1000
	test66_11 icmp-error/ptb64-test icmp-error/ptb64-expected 1,2,3
	sudo ip netns exec joolns ip link set to_client_v4 mtu 1500

	sudo ip netns exec joolns ip link set to_client_v6 mtu 1280
	test44_11 icmp-error/ptb46-test icmp-error/ptb46-expected 1,4,5,6,10,11
	sudo ip netns exec joolns ip link set to_client_v6 mtu 1500
fi

# Packet Too Big tests
# (PTB ICMP errors. Extremely important for PMTU Discovery.)
# These are the forwarding PTBs; Jool-created PTBs are tested in the 'errors'
# section above.
if [ -z "$1" -o "$1" = "ptb" ]; then
	# These packets are designed to prepare the sessions the actual tests will
	# need.
	test64_11 ptb64/session-test ptb64/session-expected $NOFRAG_IGNORE
	test64_11 ptb46/session-test ptb46/session-expected $NOFRAG_IGNORE
	test66_11 ptb66/session-test ptb66/session-expected

	# Actual tests
	test64_11 ptb64/test ptb64/expected $NOFRAG_IGNORE,$NOFRAG_IGNORE_INNER
	test46_11 ptb46/test ptb46/expected
	test66_11 ptb66/test ptb66/expected
fi

# Simultaneous Open tests
if [ -z "$1" -o "$1" = "so" ]; then
	ip netns exec client4ns $GRAYBOX expect add `dirname $0`/manual/so/46-receiver.pkt 1,4,5,6,10,11
	ip netns exec client4ns $GRAYBOX send `dirname $0`/manual/so/46-sender.pkt
	sleep 8.1 # The timer runs every two seconds.
	ip netns exec client4ns $GRAYBOX expect flush

	ip netns exec client6ns $GRAYBOX expect add `dirname $0`/manual/so/66-receiver.pkt 1,5,6,10,11
	ip netns exec client6ns $GRAYBOX send `dirname $0`/manual/so/66-sender.pkt
	sleep 8.1 # The timer runs every two seconds.
	ip netns exec client6ns $GRAYBOX expect flush

	ip netns exec client4ns $GRAYBOX expect add `dirname $0`/manual/so/success-receiver.pkt 4,5,6,10,11
	ip netns exec client4ns $GRAYBOX send `dirname $0`/manual/so/success-sender4.pkt
	ip netns exec client6ns $GRAYBOX send `dirname $0`/manual/so/success-sender6.pkt
	sleep 0.1
	ip netns exec client4ns $GRAYBOX expect flush
fi

$GRAYBOX stats display
result=$?
$GRAYBOX stats flush


echo "---------------"
echo "Strictly speaking, I'm done testing, but I'll wait 5:05 minutes."
echo "This is intended to test session timer timeout."
echo "You can see the status by running 'ip netns exec joolns jool se d --nu' in a separate terminal."

for i in $(seq 305 -1 1); do
	printf "Cleaning up in $i seconds.  \r"
	sleep 1
done
echo "---------------"


exit $result
