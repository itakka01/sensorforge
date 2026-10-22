"""Read-only SensorForge HTTP cluster discovery using a reachable seed node.

The cluster's signed multicast protocol is NOT emulated here. The cluster
password is neither necessary nor accepted for HTTP status discovery.
"""
import ipaddress
import socket
import urllib.parse


def local_node(payload):
    if not isinstance(payload, dict):
        return None
    return next((n for n in payload.get('nodes', [])
                 if isinstance(n, dict) and n.get('local')), None)


def identity(payload):
    local = local_node(payload)
    return local.get('integration_id') if local else None


def cluster_id(payload):
    return payload.get('runtime_cluster_id') or payload.get('cluster_id')


def valid_ip(ip):
    try:
        addr = ipaddress.ip_address(str(ip))
        return addr.version == 4 and not (addr.is_unspecified or addr.is_multicast or addr.is_loopback or addr.is_link_local)
    except ValueError:
        return False


def discovered_nodes(payload):
    for n in payload.get('nodes', []):
        if not isinstance(n, dict):
            continue
        uid = n.get('integration_id')
        if not isinstance(uid, str) or not uid.startswith('sf-'):
            continue
        yield uid, n


def seed_host(seed):
    if not isinstance(seed, str) or not seed.strip():
        raise ValueError('Missing seed_host')
    value = seed.strip()
    parsed = urllib.parse.urlsplit(value if '://' in value else 'http://' + value)
    if parsed.scheme != 'http' or not parsed.hostname or parsed.username or parsed.password or parsed.path not in ('', '/'):
        raise ValueError('seed_host must be a plain local HTTP hostname or IP')
    return parsed.netloc


def host_label(entry):
    return f"{entry.get('hostname') or 'node'}@{entry['id'][3:11]}"


def observe(known, payload, expected_cluster, now, events):
    if not payload or not payload.get('ok') or cluster_id(payload) != expected_cluster:
        return False
    for uid, node in discovered_nodes(payload):
        entry = known.get(uid)
        if entry is None:
            entry = {'id': uid, 'hostname': node.get('hostname') or '', 'ip': '',
                     'first_seen': now, 'last_seen': now, 'validated': False}
            known[uid] = entry
            events.append((now, uid, 'discovered', node.get('hostname') or ''))
        new_ip = node.get('ip')
        if valid_ip(new_ip) and new_ip != entry['ip']:
            events.append((now, uid, 'ip_changed', f"{entry['ip']} -> {new_ip}"))
            entry['ip'] = new_ip
        if node.get('hostname'):
            entry['hostname'] = node['hostname']
        entry['last_seen'] = now
    return True
