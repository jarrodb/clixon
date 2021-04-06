#!/usr/bin/env bash
# Restconf native using socket network namespace (netns) support
# Listen to a default and a separate netns
# Init running with a=42
# Get the config from default and netns namespace with/without SSL
# Write b=99 in netns and read from default

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

# Skip if other than native
if [ "${WITH_RESTCONF}" != "native" ]; then
    if [ "$s" = $0 ]; then exit 0; else return 0; fi # skip
fi

# Skip if valgrind restconf (actually valgrind version < 3.16 27 May 2020)
if [ $valgrindtest -eq 3 ]; then
    if [ "$s" = $0 ]; then exit 0; else return 0; fi # skip
fi

# Check if ip netns is implemented (Alpine does not have it)
ip netns 2> /dev/null
if [ $? -ne 0 ]; then
    echo "...ip netns does not work"
    if [ "$s" = $0 ]; then exit 0; else return 0; fi # skip
fi

APPNAME=example

cfg=$dir/conf.xml
startupdb=$dir/startup_db

netns=clixonnetns
veth=veth0
vethpeer=veth1
vaddr=10.23.1.1 # address in netns

# Create server certs
certdir=$dir/certs
srvkey=$certdir/srv_key.pem
srvcert=$certdir/srv_cert.pem
cakey=$certdir/ca_key.pem # needed?
cacert=$certdir/ca_cert.pem
test -d $certdir || mkdir $certdir
. ./certs.sh

# XXX Note default port need to be 80 for wait_restconf to work
RESTCONFIG=$(cat <<EOF
<restconf>
   <enable>true</enable>
   <auth-type>none</auth-type>
   <server-cert-path>$srvcert</server-cert-path>
   <server-key-path>$srvkey</server-key-path>
   <server-ca-cert-path>$cakey</server-ca-cert-path>
   <pretty>false</pretty>
   <socket>     <!-- reference and to get wait-restconf to work -->
      <namespace>default</namespace>
      <address>0.0.0.0</address>
      <port>80</port>
      <ssl>false</ssl>
   </socket>
   <!-- namespace http -->
   <socket>
      <namespace>$netns</namespace>
      <address>0.0.0.0</address>
      <port>80</port>
      <ssl>false</ssl>
   </socket>
   <!-- namespace https -->
   <socket>
      <namespace>$netns</namespace>
      <address>0.0.0.0</address>
      <port>443</port>
      <ssl>true</ssl>
   </socket>
</restconf>"
EOF
)

cat <<EOF > $cfg
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGFILE>$cfg</CLICON_CONFIGFILE>
  <CLICON_FEATURE>ietf-netconf:startup</CLICON_FEATURE>
  <CLICON_YANG_DIR>/usr/local/share/clixon</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>$IETFRFC</CLICON_YANG_DIR>
  <CLICON_YANG_MODULE_MAIN>clixon-example</CLICON_YANG_MODULE_MAIN>
  <CLICON_CLISPEC_DIR>/usr/local/lib/$APPNAME/clispec</CLICON_CLISPEC_DIR>
  <CLICON_BACKEND_DIR>/usr/local/lib/$APPNAME/backend</CLICON_BACKEND_DIR>
  <CLICON_BACKEND_REGEXP>example_backend.so$</CLICON_BACKEND_REGEXP>
  <CLICON_RESTCONF_DIR>/usr/local/lib/$APPNAME/restconf</CLICON_RESTCONF_DIR>
  <CLICON_CLI_DIR>/usr/local/lib/$APPNAME/cli</CLICON_CLI_DIR>
  <CLICON_CLI_MODE>$APPNAME</CLICON_CLI_MODE>
  <CLICON_SOCK>/usr/local/var/$APPNAME/$APPNAME.sock</CLICON_SOCK>
  <CLICON_BACKEND_PIDFILE>/usr/local/var/$APPNAME/$APPNAME.pidfile</CLICON_BACKEND_PIDFILE>
  <CLICON_XMLDB_DIR>$dir</CLICON_XMLDB_DIR>
  <CLICON_MODULE_LIBRARY_RFC7895>true</CLICON_MODULE_LIBRARY_RFC7895>
  $RESTCONFIG
</clixon-config>
EOF

new "Create netns: $netns"
sudo ip netns delete $netns
# Create netns
sudo ip netns add $netns
if [ -z "$(ip netns list | grep $netns)" ]; then
    err "$netns" "$netns does not exist"
fi

new "Create veth pair: $veth and $vethpeer"
sudo ip link delete $veth  2> /dev/null
sudo ip link delete $vethpeer 2> /dev/null
sudo ip link add $veth type veth peer name $vethpeer
if [ -z "$(ip netns show $veth)" ]; then
    err "$veth" "$veth does not exist"
fi
if [ -z "$(ip netns show $vethpeer)" ]; then
    err "$veth" "$vethpeer does not exist"
fi

new "Move $vethpeer to netns $netns"
sudo ip link set $vethpeer netns $netns
if [ -z "$( sudo ip netns exec $netns ip link show $vethpeer)" ]; then
    err "$veth" "$vethpeer does not exist"
fi

new "Assign address $vaddr on $veth in netns $netns"
sudo ip netns exec $netns ip addr add $vaddr/24 dev $vethpeer
sudo ip netns exec $netns ip link set dev $vethpeer up
sudo ip netns exec $netns ip link set dev lo up
#sudo ip netns exec $netns ping $vaddr

#-----------------

new "test params: -f $cfg"
if [ $BE -ne 0 ]; then
    new "kill old backend"
    sudo clixon_backend -z -f $cfg
    if [ $? -ne 0 ]; then
	err
    fi
    new "start backend -s init -f $cfg"
    start_backend -s init -f $cfg

    new "waiting"
    wait_backend
fi

if [ $RC -ne 0 ]; then
    new "kill old restconf daemon"
    stop_restconf_pre

    new "start restconf daemon"
    start_restconf -f $cfg

    new "waiting"
    wait_restconf # need to use port 80/443
fi

new "add sample config w netconf"
expecteof "$clixon_netconf -qf $cfg" 0 "$DEFAULTHELLO<rpc $DEFAULTNS><edit-config><target><candidate/></target><config><table xmlns=\"urn:example:clixon\"><parameter><name>a</name><value>42</value></parameter></table></config></edit-config></rpc>]]>]]>" "^<rpc-reply $DEFAULTNS><ok/></rpc-reply>]]>]]>$"

new "netconf commit"
expecteof "$clixon_netconf -qf $cfg" 0 "$DEFAULTHELLO<rpc $DEFAULTNS><commit/></rpc>]]>]]>" "^<rpc-reply $DEFAULTNS><ok/></rpc-reply>]]>]]>$"

new "restconf http get config on default netns"
expectpart "$(curl $CURLOPTS -X GET -H 'Accept: application/yang-data+xml' http://127.0.0.1/restconf/data/clixon-example:table)" 0 "HTTP/1.1 200 OK" '<table xmlns="urn:example:clixon"><parameter><name>a</name><value>42</value></parameter></table>'

new "restconf http get config on addr:$vaddr in netns:$netns"
expectpart "$(sudo ip netns exec $netns curl $CURLOPTS -X GET -H 'Accept: application/yang-data+xml' https://$vaddr/restconf/data/clixon-example:table)" 0 "HTTP/1.1 200 OK" '<table xmlns="urn:example:clixon"><parameter><name>a</name><value>42</value></parameter></table>'

new "restconf https/SSL get config on addr:$vaddr in netns:$netns"
expectpart "$(sudo ip netns exec $netns curl $CURLOPTS -X GET -H 'Accept: application/yang-data+xml' https://$vaddr/restconf/data/clixon-example:table)" 0 "HTTP/1.1 200 OK" '<table xmlns="urn:example:clixon"><parameter><name>a</name><value>42</value></parameter></table>'

new "restconf https/SSL put table b"
expectpart "$(sudo ip netns exec $netns curl $CURLOPTS -X POST -H 'Content-Type: application/yang-data+xml' -d '<parameter xmlns="urn:example:clixon"><name>b</name><value>99</value></parameter>' https://$vaddr/restconf/data/clixon-example:table)" 0 "HTTP/1.1 201 Created" 

new "restconf http get table b on default ns"
expectpart "$(curl $CURLOPTS -X GET -H 'Accept: application/yang-data+xml' http://127.0.0.1/restconf/data/clixon-example:table/parameter=b)" 0 "HTTP/1.1 200 OK" '<parameter xmlns="urn:example:clixon"><name>b</name><value>99</value></parameter>'

# Negative
new "restconf get config on wrong port in netns:$netns"
expectpart "$(sudo ip netns exec $netns curl $CURLOPTS -X GET -H 'Accept: application/yang-data+xml' $RCPROTO://$vaddr:8888/restconf/data/clixon-example:table 2> /dev/null)" 7 

if [ $RC -ne 0 ]; then
    new "Kill restconf daemon"
    stop_restconf
fi

if [ $BE -ne 0 ]; then
    new "Kill backend"
    # Check if premature kill
    pid=$(pgrep -u root -f clixon_backend)
    if [ -z "$pid" ]; then
	err "backend already dead"
    fi
    # kill backend
    stop_backend -f $cfg
fi

sudo ip link delete $veth
sudo ip netns delete $netns

new "endtest"
endtest

rm -rf $dir
