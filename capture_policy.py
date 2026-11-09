"""Pure capture identity rules. Missing evidence never implies a match."""
import re
from urllib.parse import urlsplit, parse_qs, unquote

def article_key(value):
    try:
        u = urlsplit(value if '://' in value else 'https://' + value)
        if u.scheme not in ('http', 'https') or u.hostname != 'mp.weixin.qq.com':
            return ''
        if u.path.startswith('/s/') and len(u.path.rstrip('/')) > 3:
            return 'short:' + unquote(u.path.rstrip('/')[3:])
        q = parse_qs(u.query)
        if u.path == '/s' and q.get('__biz') and (q.get('mid') or q.get('sn')):
            return 'long:' + '|'.join((q['__biz'][0], q.get('mid', q.get('sn'))[0]))
    except (ValueError, TypeError):
        pass
    return ''

def clean_title(value):
    t = re.sub(r'[​-‏﻿⁠]', '', value or '').strip()
    t = re.sub(r'\s*[-—]\s*(?:Microsoft Edge|Google Chrome|Mozilla Firefox|微信)\s*$', '', t, flags=re.I)
    t = re.sub(r'\s*[-—]\s*(?:个人|工作|Profile \d+)\s*$', '', t, flags=re.I)
    t = re.sub(r'\s*和另外\s*\d+\s*个页面.*$', '', t)
    return re.sub(r'[\W_]+', '', t, flags=re.UNICODE).casefold()

def title_matches(expected, actual):
    a, b = clean_title(expected), clean_title(actual)
    if min(len(a), len(b)) < 8:
        return False
    if a in {'微信公众平台', '西建大环境学院', 'microsoftedge'}:
        return False
    return a == b

def automatic_allowed(expected, actual, explicit=False):
    return bool(actual and (explicit or title_matches(expected, actual)))
