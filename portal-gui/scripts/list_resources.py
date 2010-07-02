#!/usr/bin/env python

import sys
import os
from datetime import datetime

from rpki.myrpki import EntityDB, CA
import rpki.config
import rpki.x509
import rpki.https
import rpki.async
import rpki.left_right
import rpki.resource_set
import rpki.ipaddrs

from rpkigui.myrpki import models

def query_rpkid(handle=None):
    """Fetch our received resources from the local rpkid using the myrpki.conf in the current directory."""
    cfg_file = os.getenv("MYRPKI_CONF", "myrpki.conf")
    cfg = rpki.config.parser(cfg_file, "myrpki")
    if handle is None:
        handle = cfg.get('handle')
    entitydb = EntityDB(cfg)
    bpki_resources = CA(cfg_file, cfg.get("bpki_resources_directory"))
    bpki_servers = CA(cfg_file, cfg.get("bpki_servers_directory"))
    rpkid_base = "https://%s:%s/" % (cfg.get("rpkid_server_host"), cfg.get("rpkid_server_port"))

    call_rpkid = rpki.async.sync_wrapper(rpki.https.caller(
        proto       = rpki.left_right,
        client_key  = rpki.x509.RSA( PEM_file = bpki_servers.dir + "/irbe.key"),
        client_cert = rpki.x509.X509(PEM_file = bpki_servers.dir + "/irbe.cer"),
        server_ta   = rpki.x509.X509(PEM_file = bpki_servers.cer),
        server_cert = rpki.x509.X509(PEM_file = bpki_servers.dir + "/rpkid.cer"),
        url         = rpkid_base + "left-right",
        debug = True))

    print 'calling rpkid... for self_handle=', handle
    rpkid_reply = call_rpkid(
        #rpki.left_right.parent_elt.make_pdu(action="list", tag="parents", self_handle=handle),
        #rpki.left_right.list_roa_requests_elt.make_pdu(tag='roas', self_handle=handle),
        rpki.left_right.child_elt.make_pdu(action="list", tag="children",
            self_handle = handle),
        rpki.left_right.list_received_resources_elt.make_pdu(tag = "resources",
            self_handle = handle))
    print 'done'

    return rpkid_reply

for pdu in query_rpkid(None if len(sys.argv) == 1 else sys.argv[1]):
    conf_set = models.Conf.objects.filter(handle=pdu.self_handle)
    if conf_set.count():
        conf = conf_set[0]
    else:
        print 'creating new conf for %s' % (pdu.self_handle,)
        conf = models.Conf.objects.create(handle=pdu.self_handle)

    #if isinstance(pdu, rpki.left_right.parent_elt):
#       print x.parent_handle, x.sia_base, x.sender_name, x.recipient_name, \
#           x.peer_contact_uri
    if isinstance(pdu, rpki.left_right.child_elt):
        # have we seen this parent before?
        child_set = conf.children.filter(handle=pdu.child_handle)
        if not child_set:
            print 'creating new child %s' % (pdu.child_handle,)
            child = models.Child(conf=conf, handle=pdu.child_handle)
            child.save()
    #elif isinstance(x, rpki.left_right.list_roa_requests_elt):
    #    print x.asn, x.ipv4, x.ipv6
    elif isinstance(pdu, rpki.left_right.list_received_resources_elt):
        # have we seen this parent before?
        parent_set = conf.parents.filter(handle=pdu.parent_handle)
        if not parent_set:
            parent = models.Parent(conf=conf, handle=pdu.parent_handle)
            parent.save()
        else:
            parent = parent_set[0]

        not_before = datetime.strptime(pdu.notBefore, "%Y-%m-%dT%H:%M:%SZ")
        not_after = datetime.strptime(pdu.notAfter, "%Y-%m-%dT%H:%M:%SZ")

        # have we seen this resource cert before?
        cert_set = parent.resources.filter(uri=pdu.uri)
        if cert_set.count() == 0:
            cert = models.ResourceCert(uri=pdu.uri, parent=parent,
                    not_before=not_before, not_after=not_after)
        else:
            cert = cert_set[0]
            # update timestamps since it could have been modified
            cert.not_before = not_before
            cert.not_after = not_after
        cert.save()

        for asn in rpki.resource_set.resource_set_as(pdu.asn):
            # see if this resource is already part of the cert
            if cert.asn.filter(lo=asn.min, hi=asn.max).count() == 0:
                # ensure this range wasn't seen from another of our parents
                for v in models.Asn.objects.filter(lo=asn.min, hi=asn.max):
                    # determine if resource is delegated from another parent
                    if v.from_cert.filter(parent__in=conf.parents.all()).count():
                        cert.asn.add(v)
                        break
                else:
                    print 'could not find ASN %s in known set' % ( asn, )
                    cert.asn.create(lo=asn.min, hi=asn.max)
                cert.save()

        # IPv4/6 - not separated in the django db
        def add_missing_address(addr_set):
           for ip in addr_set:
               lo=str(ip.min)
               hi=str(ip.max)
               if cert.address_range.filter(lo=lo, hi=hi).count() == 0:
                   # ensure that this range wasn't previously seen from another of our parents
                   for v in models.AddressRange.objects.filter(lo=lo, hi=hi):
                       # determine if this resource is delegated from another parent as well
                       if v.from_cert.filter(parent__in=conf.parents.all()).count():
                           cert.address_range.add(v)
                           break
                   else:
                       print 'could not find address range %s in known set' % (ip,)
                       cert.address_range.create(lo=lo, hi=hi)
                   cert.save()

        add_missing_address(rpki.resource_set.resource_set_ipv4(pdu.ipv4))
        add_missing_address(rpki.resource_set.resource_set_ipv6(pdu.ipv6))

# vim:sw=4 expandtab ts=4
