#!/usr/bin/env bash
# Authentication and authorization and IETF NACM
# NACM module rules
# A module rule has the "module-name" leaf set but no nodes from the
# "rule-type" choice set.
# @see test_nacm.sh is slightly modified - this follows the RFC more closely
# See RFC 8341 A.1 and A.2
# Note: use clixon-example instead of ietf-netconf-monitoring since the latter is
# A) Three tracks in the code for leaf/leaf-list, container/lists, and root
# B) Three operations: create, update, delete (write)
# C) Two access operations: permit, deny  (also default deny/permit)
# This gives 18 testcases
# Set group access:
# - Admin: permit: create, update, delete
# - Limit: permit: create, delete; deny: update
# - Guest: permit: update; deny: create delete
# ops\track:|  root  |  leaf  | list
#-----------+--------+--------+----------
# create    |  na    |  p/d   | p/d
# update    |  p/d   |  p/d   | p/d
# delete    |  p/d   |  p/d   | p/d

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

APPNAME=example

# Common NACM scripts
. ./nacm.sh

cfg=$dir/conf_yang.xml
fyang=$dir/nacm-example.yang
fyang2=$dir/clixon-example.yang

# Define default restconfig config: RESTCONFIG
RESTCONFIG=$(restconf_config user false)

cat <<EOF > $cfg
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGFILE>$cfg</CLICON_CONFIGFILE>
  <CLICON_YANG_DIR>${YANG_INSTALLDIR}</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>$dir</CLICON_YANG_MAIN_DIR>
  <CLICON_CLISPEC_DIR>/usr/local/lib/$APPNAME/clispec</CLICON_CLISPEC_DIR>
  <CLICON_RESTCONF_DIR>/usr/local/lib/$APPNAME/restconf</CLICON_RESTCONF_DIR>
  <CLICON_CLI_DIR>/usr/local/lib/$APPNAME/cli</CLICON_CLI_DIR>
  <CLICON_CLI_MODE>$APPNAME</CLICON_CLI_MODE>
  <CLICON_SOCK>/usr/local/var/$APPNAME/$APPNAME.sock</CLICON_SOCK>
  <CLICON_YANG_LIBRARY>false</CLICON_YANG_LIBRARY>
  <CLICON_BACKEND_DIR>/usr/local/lib/$APPNAME/backend</CLICON_BACKEND_DIR>
  <CLICON_BACKEND_PIDFILE>/usr/local/var/$APPNAME/$APPNAME.pidfile</CLICON_BACKEND_PIDFILE>
  <CLICON_XMLDB_DIR>/usr/local/var/$APPNAME</CLICON_XMLDB_DIR>
  <CLICON_NACM_MODE>internal</CLICON_NACM_MODE>
  <CLICON_NACM_DISABLED_ON_EMPTY>true</CLICON_NACM_DISABLED_ON_EMPTY>
  $RESTCONFIG
</clixon-config>
EOF

cat <<EOF > $fyang
module nacm-example{
  yang-version 1.1;
  namespace "urn:example:nacm";
  prefix nex;
  import clixon-example {
        prefix ex;
  }
  import ietf-netconf-acm {
        prefix nacm;
  }
  leaf x{
    type int32;
    description "something to edit";
  }
  list a{
    key k;
    leaf k{
      type string;
    }
    container b{
      leaf c{
        type string;
      }
    }
  }
}
EOF

cat <<EOF > $fyang2
module clixon-example{
  yang-version 1.1;
  namespace "urn:example:clixon";
  prefix ex;
    container table{
        list parameter{
            key name;
            leaf name{
                type string;
            }
            leaf value{
                type string;
            }
     }
   }
    /* State data (not config) for the example application*/
    container state {
         config false;
         description "state data for the example application (must be here for example get operation)";
         leaf-list op {
            type string;
         }
    }
}
EOF

# The groups are slightly modified from RFC8341 A.1 ($USER added in admin group)
# The rule-list is from A.2 
RULES=$(cat <<EOF
   <nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
     <enable-nacm>true</enable-nacm>
     <read-default>deny</read-default>
     <write-default>deny</write-default>
     <exec-default>permit</exec-default>

     $NGROUPS

     <rule-list>
       <name>limited-acl</name>
       <group>limited</group>
       <rule>
         <name>permit-create-delete</name>
         <module-name>nacm-example</module-name>
         <access-operations>read create delete</access-operations>
         <action>permit</action>
       </rule>
       <rule>
         <name>deny-update</name>
         <module-name>nacm-example</module-name>
         <access-operations>update</access-operations>
         <action>deny</action>
       </rule>
     </rule-list>

     <rule-list>
       <name>guest-acl</name>
       <group>guest</group>
       <rule>
         <name>permit-update</name>
         <module-name>nacm-example</module-name>
         <access-operations>read update</access-operations>
         <action>permit</action>
       </rule>
       <rule>
         <name>deny-create-delete</name>
         <module-name>nacm-example</module-name>
         <access-operations>create delete</access-operations>
         <action>deny</action>
       </rule>

     </rule-list>

     $NADMIN

   </nacm>
EOF
     )

new "test params: -f $cfg"

if [ $BE -ne 0 ]; then
    new "kill old backend"
    sudo clixon_backend -zf $cfg
    if [ $? -ne 0 ]; then
        err
    fi
    new "start backend -s init -f $cfg"
    start_backend -s init -f $cfg
fi

new "wait backend"
wait_backend

if [ $RC -ne 0 ]; then
    new "kill old restconf daemon"
    stop_restconf_pre

    new "start restconf daemon"
    start_restconf -f $cfg
fi

new "wait restconf"
wait_restconf

# Set nacm from scratch
function nacm(){
    new "auth set authentication config"
    expecteof_netconf "$clixon_netconf -qf $cfg" 0 "$DEFAULTHELLO" "<rpc $DEFAULTNS><edit-config><target><candidate/></target><config operation='replace'>$RULES</config></edit-config></rpc>" "" "<rpc-reply $DEFAULTNS><ok/></rpc-reply>"

    new "commit it"
    expecteof_netconf "$clixon_netconf -qf $cfg" 0 "$DEFAULTHELLO" "<rpc $DEFAULTNS><commit/></rpc>" "" "<rpc-reply $DEFAULTNS><ok/></rpc-reply>"

    new "enable nacm"
    expectpart "$(curl -u andy:bar $CURLOPTS -X PUT -H 'Content-Type: application/yang-data+json' -d '{"ietf-netconf-acm:enable-nacm":true}' $RCPROTO://localhost/restconf/data/ietf-netconf-acm:nacm/enable-nacm)" 0 "HTTP/$HVER 204"
}

#--------------- enable nacm
nacm

# ops\track:|  root  |  leaf  | list
#-----------+--------+--------+----------
# create    |  n/a   | xp/dx  |  p/d
# update    |  p/d   | xp/dx  |  p/d
# delete    |  p/d   | xp/dx  |  p/d

# replace all, then must include NACM rules as well
# This usually triggers a "HTTP/$HVER 100" from curl as well
MSG="<data>$RULES</data>"
new "update root list permit (trigger 100 Continue)"
expectpart "$(curl -u andy:bar $CURLOPTS -H 'Content-Type: application/yang-data+xml' -X PUT $RCPROTO://localhost/restconf/data -d "$MSG")" 0 "HTTP/$HVER 204"

new "delete root list deny"
expectpart "$(curl -u wilma:bar $CURLOPTS -X DELETE $RCPROTO://localhost/restconf/data)" 0 "HTTP/$HVER 403" '{"ietf-restconf:errors":{"error":{"error-type":"application","error-tag":"access-denied","error-severity":"error","error-message":"default deny"}}}'

new "delete root permit"
expectpart "$(curl -u andy:bar $CURLOPTS -X DELETE $RCPROTO://localhost/restconf/data)" 0 "HTTP/$HVER 204"

#--------------- re-enable nacm
nacm

#----------leaf
new "create leaf deny"
expectpart "$(curl -u guest:bar $CURLOPTS -H 'Content-Type: application/yang-data+xml' -X PUT $RCPROTO://localhost/restconf/data/nacm-example:x -d '<x xmlns="urn:example:nacm">42</x>')" 0 "HTTP/$HVER 403" '{"ietf-restconf:errors":{"error":{"error-type":"application","error-tag":"access-denied","error-severity":"error","error-message":"access denied"}}}'

new "create leaf permit"
expectpart "$(curl -u wilma:bar $CURLOPTS -H 'Content-Type: application/yang-data+xml' -X PUT $RCPROTO://localhost/restconf/data/nacm-example:x -d '<x xmlns="urn:example:nacm">42</x>')" 0 "HTTP/$HVER 201"

new "update leaf deny"
expectpart "$(curl -u wilma:bar $CURLOPTS -H 'Content-Type: application/yang-data+xml' -X PUT $RCPROTO://localhost/restconf/data/nacm-example:x -d '<x xmlns="urn:example:nacm">99</x>')" 0 "HTTP/$HVER 403" '{"ietf-restconf:errors":{"error":{"error-type":"application","error-tag":"access-denied","error-severity":"error","error-message":"access denied"}}}'

new "update leaf permit"
expectpart "$(curl -u guest:bar $CURLOPTS -H 'Content-Type: application/yang-data+xml' -X PUT $RCPROTO://localhost/restconf/data/nacm-example:x -d '<x xmlns="urn:example:nacm">99</x>')" 0 "HTTP/$HVER 204"

new "read leaf check"
expectpart "$(curl -u guest:bar $CURLOPTS -X GET $RCPROTO://localhost/restconf/data/nacm-example:x)" 0 "HTTP/$HVER 200" '{"nacm-example:x":99}'

new "delete leaf deny"
expectpart "$(curl -u guest:bar $CURLOPTS -X DELETE $RCPROTO://localhost/restconf/data/nacm-example:x)" 0 "HTTP/$HVER 403" '{"ietf-restconf:errors":{"error":{"error-type":"application","error-tag":"access-denied","error-severity":"error","error-message":"access denied"}}}'

new "delete leaf permit"
expectpart "$(curl -u wilma:bar $CURLOPTS -X DELETE $RCPROTO://localhost/restconf/data/nacm-example:x)" 0 "HTTP/$HVER 204"

#-----  list/container
new "create list deny"
expectpart "$(curl -u guest:bar $CURLOPTS -H 'Content-Type: application/yang-data+xml' -X PUT $RCPROTO://localhost/restconf/data/nacm-example:a=key42 -d '<a xmlns="urn:example:nacm"><k>key42</k><b><c>str</c></b></a>')" 0 "HTTP/$HVER 403" '{"ietf-restconf:errors":{"error":{"error-type":"application","error-tag":"access-denied","error-severity":"error","error-message":"access denied"}}}'

new "create list permit"
expectpart "$(curl -u wilma:bar $CURLOPTS -H 'Content-Type: application/yang-data+xml' -X PUT $RCPROTO://localhost/restconf/data/nacm-example:a=key42 -d '<a xmlns="urn:example:nacm"><k>key42</k><b><c>str</c></b></a>')" 0 "HTTP/$HVER 201"

new "update list deny"
expectpart "$(curl -u wilma:bar $CURLOPTS -H 'Content-Type: application/yang-data+xml' -X PUT $RCPROTO://localhost/restconf/data/nacm-example:a=key42 -d '<a xmlns="urn:example:nacm"><k>key42</k><b><c>update</c></b></a>')" 0 "HTTP/$HVER 403" '{"ietf-restconf:errors":{"error":{"error-type":"application","error-tag":"access-denied","error-severity":"error","error-message":"access denied"}}'

new "update list permit"
expectpart "$(curl -u guest:bar $CURLOPTS -H 'Content-Type: application/yang-data+xml' -X PUT $RCPROTO://localhost/restconf/data/nacm-example:a=key42 -d '<a xmlns="urn:example:nacm"><k>key42</k><b><c>update</c></b></a>')" 0 "HTTP/$HVER 204"

new "read list check"
expectpart "$(curl -u guest:bar $CURLOPTS -X GET $RCPROTO://localhost/restconf/data/nacm-example:a=key42)" 0 "HTTP/$HVER 200" '{"nacm-example:a":[{"k":"key42","b":{"c":"update"}}]}
'

new "delete list deny"
expectpart "$(curl -u guest:bar $CURLOPTS -X DELETE $RCPROTO://localhost/restconf/data/nacm-example:a=key42)" 0 "HTTP/$HVER 403" '{"ietf-restconf:errors":{"error":{"error-type":"application","error-tag":"access-denied","error-severity":"error","error-message":"access denied"}}}'

new "delete list permit"
expectpart "$(curl -u wilma:bar $CURLOPTS -X DELETE $RCPROTO://localhost/restconf/data/nacm-example:a=key42)" 0 "HTTP/$HVER 204"

#----- default deny (clixon-example limit and guest have default access)
new "default create list deny"
expectpart "$(curl -u wilma:bar $CURLOPTS -X PUT -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-example:table/parameter=key42 -d '{"clixon-example:parameter":[{"name":"key42","value":"val42"}]}')" 0 "HTTP/$HVER 403" '{"ietf-restconf:errors":{"error":{"error-type":"application","error-tag":"access-denied","error-severity":"error","error-message":"default deny"}}'

new "create list permit"
expectpart "$(curl -u andy:bar $CURLOPTS -X PUT -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-example:table/parameter=key42 -d '{"clixon-example:parameter": [{"name":"key42","value":"val42"}]}')" 0 "HTTP/$HVER 201"

new "default update list deny"
expectpart "$(curl -u wilma:bar $CURLOPTS -X PUT -H "Content-Type: application/yang-data+json" $RCPROTO://localhost/restconf/data/clixon-example:table/parameter=key42 -d '{"clixon-example:parameter": [{"name":"key42","value":"val99"}]}')" 0 "HTTP/$HVER 403" '{"ietf-restconf:errors":{"error":{"error-type":"application","error-tag":"access-denied","error-severity":"error","error-message":"default deny"}}'

new "default delete list deny"
expectpart "$(curl -u wilma:bar $CURLOPTS -X DELETE $RCPROTO://localhost/restconf/data/clixon-example:table/parameter=key42)" 0 "HTTP/$HVER 403" '{"ietf-restconf:errors":{"error":{"error-type":"application","error-tag":"access-denied","error-severity":"error","error-message":"default deny"}}'

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

# Set by restconf_config
unset RESTCONFIG

rm -rf $dir

new "endtest"
endtest
