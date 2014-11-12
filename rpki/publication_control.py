# $Id$
#
# Copyright (C) 2013--2014  Dragon Research Labs ("DRL")
# Portions copyright (C) 2009--2012  Internet Systems Consortium ("ISC")
# Portions copyright (C) 2007--2008  American Registry for Internet Numbers ("ARIN")
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notices and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND DRL, ISC, AND ARIN DISCLAIM ALL
# WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL DRL,
# ISC, OR ARIN BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
# CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
# OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
# NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
# WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

"""
RPKI publication control protocol.

Per IETF SIDR WG discussion, this is now separate from the publication
protocol itself.
"""

import logging
import rpki.resource_set
import rpki.x509
import rpki.sql
import rpki.exceptions
import rpki.xml_utils
import rpki.http
import rpki.up_down
import rpki.relaxng
import rpki.sundial
import rpki.log

logger = logging.getLogger(__name__)


nsmap    = rpki.relaxng.publication_control.nsmap
version  = rpki.relaxng.publication_control.version

tag_msg           = rpki.relaxng.publication_control.xmlns + "msg"
tag_client        = rpki.relaxng.publication_control.xmlns + "client"
tag_bpki_cert     = rpki.relaxng.publication_control.xmlns + "bpki_cert"
tag_bpki_glue     = rpki.relaxng.publication_control.xmlns + "bpki_glue"
tag_report_error  = rpki.relaxng.publication_control.xmlns + "report_error"


def raise_if_error(pdu):
  """
  Raise an appropriate error if this is a <report_error/> PDU.

  As a convience, this will also accept a <msg/> PDU and raise an
  appropriate error if it contains any <report_error/> PDUs.
  """

  if pdu.tag == tag_report_error:
    code = pdu.get("error_code")
    logger.debug("<report_error/> code %r", code)
    e = getattr(rpki.exceptions, code, None)
    if e is not None and issubclass(e, rpki.exceptions.RPKI_Exception):
      raise e(pdu.text)
    else:
      raise rpki.exceptions.BadPublicationReply("Unexpected response from pubd: %r, %r" % (code, pdu))

  if pdu.tag == tag_msg:
    for p in pdu:
      raise_if_error(p)


class publication_control_namespace(object):
  """
  XML namespace parameters for publication control protocol.
  """

  xmlns = rpki.relaxng.publication_control.xmlns
  nsmap = rpki.relaxng.publication_control.nsmap


class client_elt(rpki.xml_utils.data_elt, rpki.sql.sql_persistent, publication_control_namespace):
  """
  <client/> element.
  """

  element_name = "client"
  attributes = ("action", "tag", "client_handle", "base_uri")
  elements = ("bpki_cert", "bpki_glue")
  booleans = ("clear_replay_protection",)

  sql_template = rpki.sql.template(
    "client",
    "client_id",
    "client_handle",
    "base_uri",
    ("bpki_cert", rpki.x509.X509),
    ("bpki_glue", rpki.x509.X509),
    ("last_cms_timestamp", rpki.sundial.datetime))

  base_uri  = None
  bpki_cert = None
  bpki_glue = None
  last_cms_timestamp = None

  def __repr__(self):
    return rpki.log.log_repr(self, self.client_handle, self.base_uri)

  @property
  def objects(self):
    return rpki.pubd.object_obj.sql_fetch_where(self.gctx, "client_id = %s", (self.client_id,))

  def serve_post_save_hook(self, q_pdu, r_pdu, cb, eb):
    """
    Extra server actions for client_elt.
    """

    if q_pdu.clear_replay_protection:
      self.last_cms_timestamp = None
      self.sql_mark_dirty()
    cb()

  def serve_fetch_one_maybe(self):
    """
    Find the client object on which a get, set, or destroy method
    should operate, or which would conflict with a create method.
    """

    return self.sql_fetch_where1(self.gctx, "client_handle = %s", (self.client_handle,))

  def serve_fetch_all(self):
    """
    Find client objects on which a list method should operate.
    """

    return self.sql_fetch_all(self.gctx)

  def check_allowed_uri(self, uri):
    """
    Make sure that a target URI is within this client's allowed URI space.
    """

    if not uri.startswith(self.base_uri):
      raise rpki.exceptions.ForbiddenURI

  def raise_if_error(self):
    """
    No-op, because this isn't a <report_error/> PDU.
    """

    pass


class report_error_elt(rpki.xml_utils.text_elt, publication_control_namespace):
  """
  <report_error/> element.
  """

  element_name = "report_error"
  attributes = ("tag", "error_code")
  text_attribute = "error_text"

  error_text = None

  @classmethod
  def from_exception(cls, e, tag = None):
    """
    Generate a <report_error/> element from an exception.
    """

    self = cls()
    self.tag = tag
    self.error_code = e.__class__.__name__
    self.error_text = str(e)
    return self

  def __str__(self):
    s = ""
    if getattr(self, "tag", None) is not None:
      s += "[%s] " % self.tag
    s += self.error_code
    if getattr(self, "error_text", None) is not None:
      s += ": " + self.error_text
    return s

  def raise_if_error(self):
    """
    Raise exception associated with this <report_error/> PDU.
    """

    t = getattr(rpki.exceptions, self.error_code, None)
    if isinstance(t, type) and issubclass(t, rpki.exceptions.RPKI_Exception):
      raise t(getattr(self, "text", None))
    else:
      raise rpki.exceptions.BadPublicationReply("Unexpected response from pubd: %s" % self)


class msg(rpki.xml_utils.msg, publication_control_namespace):
  """
  Publication control PDU.
  """

  ## @var version
  # Protocol version
  version = int(rpki.relaxng.publication_control.version)

  ## @var pdus
  # Dispatch table of PDUs for this protocol.
  pdus = dict((x.element_name, x) for x in (client_elt, report_error_elt))

  def serve_top_level(self, gctx, cb):
    """
    Serve one msg PDU.
    """

    if not self.is_query():
      raise rpki.exceptions.BadQuery("Message type is not query")
    r_msg = self.__class__.reply()

    for q_pdu in self:

      def next():
        # Relic of asynch I/O structure, clean up eventually
        pass

      def fail(e):
        if not isinstance(e, rpki.exceptions.NotFound):
          logger.exception("Exception processing PDU %r", q_pdu)
        r_msg.append(report_error_elt.from_exception(e, q_pdu.tag))
        return cb(r_msg)

      try:
        q_pdu.gctx = gctx
        q_pdu.serve_dispatch(r_msg, next, fail)
      except Exception, e:
        return fail(e)

    return cb(r_msg)


class sax_handler(rpki.xml_utils.sax_handler):
  """
  SAX handler for publication control protocol.
  """

  pdu = msg
  name = "msg"
  version = rpki.relaxng.publication_control.version


class cms_msg(rpki.x509.XML_CMS_object):
  """
  Class to hold a CMS-signed publication control PDU.
  """

  encoding = "us-ascii"
  schema = rpki.relaxng.publication_control
  saxify = sax_handler.saxify


class cms_msg_no_sax(cms_msg):
  """
  Class to hold a CMS-signed publication control PDU without legacy
  SAX transcoding.  The name is a transition kludge, this class will
  be renamed cms_msg once the SAX code goes away.
  """

  saxify = None