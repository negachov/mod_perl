package TestProtocol::echo_block;

# this test reads from/writes to the socket doing blocking IO
#
# see TestProtocol::echo_timeout for how to do the same with
# nonblocking IO but using the timeout

use strict;
use warnings FATAL => 'all';

use Apache::Connection ();
use APR::Socket ();

use Apache::Const -compile => 'OK';
use APR::Const    -compile => qw(SO_NONBLOCK);

use constant BUFF_LEN => 1024;

sub handler {
    my Apache::Connection $c = shift;
    my APR::Socket $socket = $c->client_socket;

    # make sure the socket is in the blocking mode for recv().
    # on some platforms (e.g. OSX/Solaris) httpd hands us a
    # non-blocking socket
    my $nonblocking = $socket->opt_get(APR::SO_NONBLOCK);
    if ($nonblocking) {
        $socket->opt_set(APR::SO_NONBLOCK => 0);

        # test that we really are in the non-blocking mode
        $socket->opt_get(APR::SO_NONBLOCK)
            or die "failed to set non-blocking mode";
    }

    my ($buff, $rlen, $wlen);
    for (;;) {
        $rlen = BUFF_LEN;
        $socket->recv($buff, $rlen);
        last if $rlen <= 0;

        $wlen = $rlen;
        $socket->send($buff, $wlen);

        last if $wlen != $rlen;
    }

    Apache::OK;
}

1;