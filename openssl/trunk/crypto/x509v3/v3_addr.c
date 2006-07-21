/*
 * Copyright (C) 2006  American Registry for Internet Numbers ("ARIN")
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ARIN DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ARIN BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/* $Id$ */

/*
 * Initial attempt to implement RFC 3779 section 2.  I'd be very
 * surprised if this even compiled yet, as I'm still figuring out
 * OpenSSL's ASN.1 template goop.
 */

#include <stdio.h>
#include <assert.h>
#include "cryptlib.h"
#include <openssl/conf.h>
#include <openssl/asn1.h>
#include <openssl/asn1t.h>
#include <openssl/x509v3.h>

ASN1_SEQUENCE(IPAddressRange) = {
  ASN1_SIMPLE(IPAddressRange, min, ASN1_BIT_STRING),
  ASN1_SIMPLE(IPAddressRange, max, ASN1_BIT_STRING)
} ASN1_SEQUENCE_END(IPAddressRange)

ASN1_CHOICE(IPAddressOrRange) = {
  ASN1_SIMPLE(IPAddressOrRange, u.addressPrefix, ASN1_BIT_STRING),
  ASN1_SIMPLE(IPAddressOrRange, u.addressRange,  IPAddressRange)
} ASN1_CHOICE_END(IPAddressOrRange)

ASN1_CHOICE(IPAddressChoice) = {
  ASN1_SIMPLE(IPAddressChoice,      u.inherit,           ASN1_NULL),
  ASN1_SEQUENCE_OF(IPAddressChoice, u.addressesOrRanges, IPAddressOrRange)
} ASN1_CHOICE_END(IPAddressChoice)

ASN1_SEQUENCE(IPAddressFamily) = {
  ASN1_SIMPLE(IPAddressFamily,      addressFamily,   ASN1_OCTET_STRING),
  ASN1_SEQUENCE_OF(IPAddressFamily, ipAddressChoice, IPAddressChoice)
} ASN1_SEQUENCE_END(IPAddressFamily)

ASN1_ITEM_TEMPLATE(IPAddrBlocks) = 
  ASN1_EX_TEMPLATE_TYPE(ASN1_TFLG_SEQUENCE_OF, 0,
			IPAddrBlocks, IPAddressFamily)
ASN1_ITEM_TEMPLATE_END(IPAddrBlocks)

IMPLEMENT_ASN1_FUNCTIONS(IPAddressRange)
IMPLEMENT_ASN1_FUNCTIONS(IPAddressOrRange)
IMPLEMENT_ASN1_FUNCTIONS(IPAddressChoice)
IMPLEMENT_ASN1_FUNCTIONS(IPAddressFamily)
IMPLEMENT_ASN1_FUNCTIONS(IPAddrBlocks)

/*
 * How much buffer space do we need for a raw address?
 */
#define ADDR_RAW_BUF_LEN	16

/*
 * How much buffer space do we need for the text form of an address?
 * Output routines (inet_ntop() or whatever) must check for overflow.
 */
#define ADDR_TXT_BUF_LEN	48

/*
 * Expand the bitstring form of an address into a raw byte array.
 * At the moment this is coded for simplicity, not speed.
 */
static void addr_expand(unsigned char *addr,
			const ASN1_BIT_STRING *bs,
			const int length,
			const unsigned char fill)
{
  assert(bs->length >= 0 && bs->length <= length);
  memset(addr, fill, length);
  if (bs->length > 0) {
    memcpy(addr, bs->data, bs->length);
    if ((bs->flags & 7) != 0)
      addr[bs->length - 1] |= fill >> (8 - (bs->flags & 7));
  }
}

/*
 * Compare two addresses.
 * At the moment this is coded for simplicity, not for speed.
 */
static int addr_cmp(const ASN1_BIT_STRING * const *a,
		    const ASN1_BIT_STRING * const *b,
		    const unsigned char fill_a,
		    const unsigned char fill_b,
		    const int length)
{
  unsigned char a_[ADDR_RAW_BUF_LEN];
  unsigned char b_[ADDR_RAW_BUF_LEN];
  assert(length <= ADDR_RAW_BUF_LEN);
  addr_expand(a_, a, length, fill_a);
  addr_expand(b_, b, length, fill_b);
  return memcmp(a, b, length);
}

static int i2r_address(BIO *out,
		       unsigned afi,
		       unsigned char fill,
		       ASN1_BIT_STRING *bs)
{
  unsigned char addr[ADDR_RAW_BUF_LEN];
  char buf[ADDR_TXT_BUF_LEN];
  int i;

  switch (afi) {
  case IANA_AFI_IPV4:
    addr_expand(addr, bs, 4, fill);
    if (inet_ntop(AF_INET, addr, buf, sizeof(buf)) == NULL)
      return 0;
    BIO_puts(out, buf);
    break;
  case IANA_AFI_IPV6:
    addr_expand(addr, bs, 16, fill);
    if (inet_ntop(AF_INET6, addr, buf, sizeof(buf)) == NULL)
      return 0;
    BIO_puts(out, buf);
    break;
  default:
    for (i = 0; i < bs->length; i++)
      BIO_printf(out, "%s%02x", (i > 0 ? ":" : ""), bs->data[i]);
    BIO_printf(out, "[%d]", bs->flags & 7);
    break;
  }
  return 1;
}

static int i2r_IPAddressOrRange(BIO *out,
				int indent,
				IPAddressOrRanges *aors,
				unsigned afi)
{
  int i;
  for (i = 0; i < sk_IPAddressOrRange_num(aors); i++) {
    IPAddressOrRange *aor = sk_IPAddressOrRange_num(aors, i);
    BIO_printf(out, "%*s", indent, "");
    switch (aor->type) {
    case IPAddressOrRange_addressPrefix:
      if (!i2r_address(out, afi, 0x00, aor->addressPrefix))
	return 0;
      BIO_printf(out, "/%d\n", 
		 aor->addressPrefix->length * 8 -
		 (aor->addressPrefix->flags & 7));
      continue;
    case IPAddressOrRange_addressRange:
      if (!i2r_address(out, afi, 0x00, aor->addressRange->min))
	return 0;
      BIO_puts(out, "-");
      if (!i2r_address(out, afi, 0xFF, aor->addressRange->max))
	return 0;
      BIO_puts(out, "\n");
      continue;
    }
  }
  return 1;
}

static int i2r_IPAddrBlocks(X509V3_EXT_METHOD *method,
			    void *ext, BIO *out, int indent)
{
  int i;
  for (i = 0; i < sk_IPAddrBlocks_num(ext); i++) {
    IPAddressFamily *f = sk_IPAddrBlocks_value(ext, i);
    unsigned afi = ((f->addressFamily->data[0] << 8) |
		    f->addressFamily->data[1]);
    switch (afi) {
    case IANA_AFI_IPV4:
      BIO_printf(out, "%*sIPv4", indent, "");
      break;
    case IANA_AFI_IPV6:
      BIO_printf(out, "%*sIPv6", indent, "");
      break;
    default:
      BIO_printf(out, "%*sUnknown AFI %u", indent, "", afi);
      break;
    }
    if (f->addressFamily->length > 2) {
      switch (f->addressFamily->data[2]) {
      case   1:
	BIO_puts(out, " (Unicast)");
	break;
      case   2:
	BIO_puts(out, " (Multicast)");
	break;
      case   3:
	BIO_puts(out, " (Unicast/Multicast)");
	break;
      case   4:
	BIO_puts(out, " (MPLS)");
	break;
      case  64:
	BIO_puts(out, " (Tunnel)");
	break;
      case  65:
	BIO_puts(out, " (VPLS)");
	break;
      case  66:
	BIO_puts(out, " (BGP MDT)");
	break;
      case 128:
	BIO_puts(out, " (MPLS-labeled VPN)");
	break;
      default:  
	BIO_printf(out, " (Unknown SAFI %u)",
		   (unsigned) f->addressFamily->data[2]);
	break;
      }
    }
    switch (f->ipAddressChoice->type) {
    case IPAddressChoice_inherit:
      BIO_puts(out, ": inherit\n");
      break;
    case IPAddressChoice_addressesOrRanges:
      BIO_puts(out, ":\n");
      if (!i2r_IPAddressOrRanges(out,
				 indent + 2,
				 f->ipAddressChoice->u.asIdsOrRanges,
				 afi))
	return 0;
      break;
    }
  }
  return 1;
}

/*
 * Compare two IPAddressOrRanges elements.
 */
static int IPAddressOrRange_cmp(const IPAddressOrRange * const *a,
				const IPAddressOrRange * const *b,
				const int length)
{
  const ASN1_BIT_STRING *addr_a, *addr_b;
  unsigned prefixlen_a, prefixlen_b;
  int r;

  switch (a->type) {
  case IPAddressOrRange_addressPrefix:
    addr_a = a->addressPrefix;
    prefixlen_a = (a->addressPrefix->length * 8 -
		   (a->addressPrefix->flags & 7));
    break;
  case IPAddressOrRange_addressRange:
    addr_a = a->addressRange->min;
    prefixlen_a = length * 8;
    break;
  }

  switch (b->type) {
  case IPAddressOrRange_addressPrefix:
    addr_b = b->addressPrefix;
    prefixlen_b = (b->addressPrefix->length * 8 -
		   (b->addressPrefix->flags & 7));
    break;
  case IPAddressOrRange_addressRange:
    addr_b = b->addressRange->min;
    prefixlen_b = length * 8;
    break;
  }

  if ((r = addr_cmp(addr_a, addr_b, 0x00, 0x00, length)) != 0)
    return r;
  else
    return prefixlen_a - prefixlen_b;
}

/*
 * Closures, since sk_sort() comparision routines are only allowed two
 * arguments.
 * 
 */
static int v4IPAddressOrRange_cmp(const IPAddressOrRange * const *a,
				  const IPAddressOrRange * const *b)
{
  return IPAddressOrRange_cmp(a, b, 4);
}

static int v6IPAddressOrRange_cmp(const IPAddressOrRange * const *a,
				  const IPAddressOrRange * const *b)
{
  return IPAddressOrRange_cmp(a, b, 16);
}

/*
 * Whack a IPAddressOrRanges into canonical form.
 */
static int IPAddressOrRanges_canonize(IPAddressOrRanges *aors,
				      unsigned afi)
{
  int i, length;

  switch (afi) {
  case IANA_AFI_IPV4:
    length = 4;
    break;
  case IANA_AFI_IPV6:
    length = 16;
    break;
  }

  sk_IPAddressOrRange_sort(aors);

  /*
   * Resolve any duplicates or overlaps.
   */

  for (i = 0; i < sk_IPAddressOrRange_num(aors) - 1; i++) {
    IPAddressOrRange *a = sk_IPAddressOrRange_value(aors, i);
    IPAddressOrRange *b = sk_IPAddressOrRange_value(aors, i + 1);

#error not right yet
    /*
     * The following tests look for overlap, but do not check for
     * adjacency.  How to implement?  Use bignums?  Yum.  All we
     * really need is the ability to add or subtract 1 from a
     * bitvector, which isn't very hard, so that's probably the plan.
     *
     * Hmm, it would also be good if I checked the ->type variables,
     * doh.
     */

    /*
     * Comparing prefix a with prefix b.  Prefixes can't overlap, only
     * nest, so we just have to check whether a contains b.
     */
    if (a->type == IPAddressOrRange_addressPrefix &&
	b->type == IPAddressOrRange_addressPrefix) {
      if (addr_cmp(a->addressPrefix, b->addressPrefix,
		   0xFF, 0xFF, length) >= 0) {
	sk_IPAddressOrRange_delete(aors, i + 1);
	ASN1_BIT_STRING_free(b->addressPrefix);
	IPAddressOrRange_free(b);
	i--;
      }
      continue;
    }

#error but prefixes can be adjacent, in which case we should merge them into a range

    /*
     * Comparing prefix a with range b.  If they overlap, we merge
     * them into a range.
     */
    if (a->type == IPAddressOrRange_addressPrefix) {
      if (addr_cmp(a->addressPrefix, b->addressRange->min,
		   0xFF, 0x00, length) >= 0) {
	sk_IPAddressOrRange_delete(aors, i);
	ASN_BIT_STRING_free(b->addressRange->min);
	b->addressRange->min = a->addressPrefix;
	IPAddressRange(a->addressRange);
	IPAddressOrRange_free(a);
	i--;
      }
      continue;
    }

    /*
     * Comparing range a with prefix b.  If they overlap, we merge
     * them into a range.
     */
    if (b->type == IPAddressOrRange_addressPrefix) {
      if (addr_cmp(a->addressRange->max, b->addressPrefix,
		   0xFF, 0x00, length) >= 0) {
	sk_IPAddressOrRange_delete(aors, i + 1);
	ASN_BIT_STRING_free(a->addressRange->max);
	a->addressRange->max = b->addressPrefix;
	IPAddressRange(b->addressRange);
	IPAddressOrRange_free(b);
	i--;
      }
      continue;
    }

    /*
     * Comparing range a with range b, remove b if contained in a.
     */
    if (addr_cmp(a->addressRange->max, b->addressRange->max,
		 0xFF, 0xFF, length) >= 0) {
      sk_IPAddressOrRange_delete(aors, i + 1);
      ASN_BIT_STRING_free(b->addressRange->min);
      ASN_BIT_STRING_free(b->addressRange->max);
      IPAddressRange(b->addressRange);
      IPAddressOrRange_free(b);
      i--;
      continue;
    }

    /*
     * Comparing range a with range b, merge if they overlap.
     */
    if (addr_cmp(a->addressRange->max, b->addressRange->min,
		 0xFF, 0x00, length) >= 0) {
      sk_IPAddressOrRange_delete(aors, i);
      ASN_BIT_STRING_free(a->addressRange->max);
      ASN_BIT_STRING_free(b->addressRange->min);
      b->addressRange->min = a->addressRange->max;
      IPAddressRange(a->addressRange);
      IPAddressOrRange_free(a);
      i--;
      continue;
    }
  }

  /*
   * Convert ranges to prefixes where possible.
   */
  for (i = 0; i < sk_IPAddressOrRange_num(aors); i++) {
    IPAddressOrRange *a = sk_IPAddressOrRange_value(aors, i);
    if (a->type == IPAddressOrRange_addressRange &&
	addr_cmp(a->addressRange->min,a->addressRange->max,
		 0x00, 0x00, length) == 0) {
      IPAddressRange *r = a->addressRange;
      a->type = IPAddressOrRange_addressPrefix;
      a->u.addressPrefix = r->min;
      ASN1_BIT_STRING_free(r->max);
      IPAddressRange_free(r);
    }
  }
}

X509V3_EXT_METHOD v3_addr = {
  NID_IPAddrBlocks,		/* nid */
  0,				/* flags */
  ASN1_ITEM_ref(IPAddrBlocks),	/* template */
  0, 0, 0, 0,			/* old functions, ignored */
  0,				/* i2s */
  0,				/* s2i */
  0,				/* i2v */
  v2i_IPAddrBlocks,		/* v2i */
  i2r_IPAddrBlocks,		/* i2r */
  0,				/* r2i */
  NULL				/* extension-specific data */
};
