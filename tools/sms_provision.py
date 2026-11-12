#!/usr/bin/env python3
"""Operator-style "settings over the air" for the virtual SIM: an OMA Client Provisioning message as SMS.

Builds what an operator sends to configure a phone's internet access point: a provisioning document
(OMA-WAP-ProvCont, WBXML) with one NAPDEF (bearer GPRS, APN) linked to the browser (APPLICATION w2), in a WSP
connectionless push (content type application/vnd.wap.connectivity-wbxml) to WAP push port 2948, split into
8-bit SMS-DELIVER TPDUs with port addressing and concatenation headers (3GPP TS 23.040). The phone shows
"configuration message received"; saving it creates the access point in the Internet destination. No PIN by default
(no security parameters); `--pin 1234` adds USERPIN authentication (SEC=USERPIN + MAC) as some operators send it.

  tools/sms_provision.py [--apn internet] [--name Internet] [--from 12345] [--pin PIN]     -> one TPDU per line (hex)

The fake modem delivers the TPDUs as incoming SMS (syborg_modem.c, QOM property "sms" of /machine/modem);
`sydb sms-provision` does that once per instance.

Token values from the OMA Provisioning Content 1.1 WBXML tables (code page 0: NAPDEF parms, code page 1:
APPLICATION parms). Values are sent as inline strings, which every WBXML parser accepts.
"""
import argparse
import time

END, SWITCH_PAGE, STR_I = 0x01, 0x00, 0x03
TAG_DOC, TAG_CHAR, TAG_PARM = 0x05, 0x06, 0x07
CONTENT, ATTRS = 0x40, 0x80
# attribute-start tokens
A_VERSION_10 = 0x46
CP0 = {'NAME': 0x07, 'NAP-ADDRESS': 0x08, 'NAP-ADDRTYPE': 0x09, 'BEARER': 0x10, 'NAPID': 0x11, 'INTERNET': 0x14,
       'type=NAPDEF': 0x55, 'value': 0x06}
CP1 = {'NAME': 0x07, 'TO-NAPID': 0x22, 'APPID': 0x36, 'type=APPLICATION': 0x55, 'value': 0x06}


def istr(s):
    if '\x00' in s:
        raise ValueError('inline string with a NUL: %r' % s)
    return bytes([STR_I]) + s.encode('utf-8') + b'\x00'


def parm(page, name, value):
    """<parm name=.. value=..>: a known value as its token (int), else an inline string; value None = no value"""
    d = bytearray([TAG_PARM | ATTRS, page[name]])
    if isinstance(value, int):
        d += bytes([page['value'], value])
    elif value is not None:
        d += bytes([page['value']]) + istr(value)
    return bytes(d + bytes([END]))


V_APN, V_GSM_GPRS = 0x89, 0xAB                   # attribute value tokens (code page 0)


def provisioning_doc(apn, name):
    """APPLICATION (browser w2, linked to the NAPDEF) first, then the NAPDEF with INTERNET: as operators' messages
    are laid out; S60's AP adapter counted zero items for the earlier order/inline-value version (the message was
    then dropped as empty, docs/research/n8-oma-cp-acceptance.md)"""
    d = bytearray([0x03, 0x0B, 0x6A, 0x00])      # WBXML 1.3, -//WAPFORUM//DTD PROV 1.0//EN, UTF-8, no string table
    d += bytes([TAG_DOC | CONTENT | ATTRS, A_VERSION_10, END])
    # <characteristic type="APPLICATION"> APPID w2 (browser), NAME, TO-NAPID NAP1  (code page 1)
    d += bytes([SWITCH_PAGE, 0x01, TAG_CHAR | CONTENT | ATTRS, CP1['type=APPLICATION'], END])
    for k, v in (('APPID', 'w2'), ('NAME', name), ('TO-NAPID', 'NAP1')):
        d += parm(CP1, k, v)
    d += bytes([END, SWITCH_PAGE, 0x00])
    # <characteristic type="NAPDEF"> NAPID, NAME, INTERNET, BEARER GSM-GPRS, NAP-ADDRESS apn, NAP-ADDRTYPE APN
    d += bytes([TAG_CHAR | CONTENT | ATTRS, CP0['type=NAPDEF'], END])
    for k, v in (('NAPID', 'NAP1'), ('NAME', name), ('INTERNET', None), ('BEARER', V_GSM_GPRS),
                 ('NAP-ADDRESS', apn), ('NAP-ADDRTYPE', V_APN)):
        d += parm(CP0, k, v)
    d += bytes([END])
    d += bytes([END])                             # </wap-provisioningdoc>
    return bytes(d)


def wsp_push(body, pin=None):
    """connectionless push: TID, PDU type Push (06), headers length, then the content type
    application/vnd.wap.connectivity-wbxml (0x36|0x80) with the OMA CP security parameters SEC=USERPIN (0x11,
    short-integer 1) and MAC (0x12, 40 hex digits of HMAC-SHA1(key = the PIN, data = the WBXML document), OMA
    ProvBoot): the phone asks for the PIN before saving. pin=None (default) sends no security parameters. (On
    2026-11-11 the N8 dropped an unauthenticated message, but that was before the document layout fix of 6d08742.)"""
    if pin is None:
        ctype = bytes([0xB6])
    else:
        import hmac, hashlib
        mac = hmac.new(pin.encode(), body, hashlib.sha1).hexdigest().upper().encode() + b'\x00'
        params = bytes([0xB6, 0x91, 0x81, 0x92]) + mac     # SEC (0x11|0x80) = USERPIN, MAC (0x12|0x80) = text
        n = len(params)                                    # general form: value length + media + params; a length
        ctype = (bytes([n]) if n < 31 else bytes([0x1F, n])) + params   # over 30 is length-quote 0x1F + uintvar (< 128)
    return bytes([0x01, 0x06, len(ctype)]) + ctype + body


def bcd_number(digits):
    s = digits + ('F' if len(digits) % 2 else '')
    return bytes(int(s[i + 1] + s[i], 16) for i in range(0, len(s), 2))


def scts():
    t = time.gmtime()
    f = lambda v: int(('%02d' % v)[::-1], 16)  # semi-octets, low digit first
    return bytes([f(t.tm_year % 100), f(t.tm_mon), f(t.tm_mday), f(t.tm_hour), f(t.tm_min), f(t.tm_sec), 0x00])


def sms_deliver_tpdus(payload, sender, ref=None):
    """8-bit SMS-DELIVER TPDUs carrying payload to WAP push port 2948; ref = the concatenation reference (default: a
    fresh random one, so two messages sent in a row are not merged by the phone)"""
    import random
    if not sender.isdigit() or not 1 <= len(sender) <= 20:
        raise ValueError('sender %r: 1..20 digits' % sender)
    ref = random.randrange(256) if ref is None else ref & 0xff
    # UDH: 16-bit application port addressing (dst 2948 WAP push, src 9200) + 8-bit concatenation
    ports = bytes([0x05, 0x04, 0x0B, 0x84, 0x23, 0xF0])
    room = 140 - 1 - len(ports) - 5               # UDHL + ports + concat IE
    parts = [payload[i:i + room] for i in range(0, len(payload), room)] or [b'']
    if len(parts) > 255:
        raise ValueError('payload of %d bytes needs %d SMS (255 at most)' % (len(payload), len(parts)))
    out = []
    for n, part in enumerate(parts, 1):
        udh = ports + (bytes([0x00, 0x03, ref, len(parts), n]) if len(parts) > 1 else b'')
        ud = bytes([len(udh)]) + udh + part
        oa = bytes([len(sender), 0x81]) + bcd_number(sender)
        tpdu = bytes([0x44]) + oa + bytes([0x00, 0x04]) + scts() + bytes([len(ud)]) + ud
        out.append(tpdu)                           # first octet: SMS-DELIVER, more-messages off, UDHI
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--apn', default='internet')
    ap.add_argument('--name', default='Internet')
    ap.add_argument('--from', dest='sender', default='12345')
    ap.add_argument('--pin', default=None, help='USERPIN authentication with this PIN (default: none)')
    a = ap.parse_args()
    for t in sms_deliver_tpdus(wsp_push(provisioning_doc(a.apn, a.name), a.pin), a.sender):
        print(t.hex())


if __name__ == '__main__':
    main()
