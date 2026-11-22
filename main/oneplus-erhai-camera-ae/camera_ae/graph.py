# SPDX-License-Identifier: GPL-2.0-only
"""The PipeWire graph from `pw-dump --monitor -N`.

The monitor prints one full dump, then delta arrays: each object replaces
its id's entry, and {"id": N, "info": null} removes it. Arrays are
pretty-printed and run together on the stream, and a read can end in the
middle of an array or of a UTF-8 character.
"""
import codecs
import json
from dataclasses import dataclass

OWN_NODE_NAME = 'camera-tone'
BUFFER_CAP = 8 * 1024 * 1024
SUFFIXES = {'camera-sensor_10': 'front', 'camera-sensor_36': 'rear'}


def sensor_kind(node_name):
    for suffix, kind in SUFFIXES.items():
        if node_name.endswith(suffix):
            return kind
    return None


@dataclass(frozen=True)
class CameraNode:
    id: int
    serial: int
    name: str
    kind: str | None
    in_use: bool


class Graph:
    def __init__(self):
        self.objects = {}
        self._decoder = codecs.getincrementaldecoder('utf-8')(errors='replace')
        self._json = json.JSONDecoder()
        self._buf = ''

    def feed(self, chunk):
        self._buf += self._decoder.decode(chunk)
        while True:
            text = self._buf.lstrip()
            if not text:
                self._buf = ''
                return
            try:
                value, end = self._json.raw_decode(text)
            except json.JSONDecodeError:
                self._buf = text if len(text) <= BUFFER_CAP else ''
                return
            self._buf = text[end:]
            if isinstance(value, list):
                for obj in value:
                    self._merge(obj)

    def _merge(self, obj):
        if not isinstance(obj, dict) or 'id' not in obj:
            return
        if obj.get('info') is None:
            self.objects.pop(obj['id'], None)
        else:
            self.objects[obj['id']] = obj

    def _props(self, obj):
        return (obj.get('info') or {}).get('props') or {}

    def _is_ours(self, node_id, own_pids):
        node = self.objects.get(node_id)
        if node is None:
            return False
        props = self._props(node)
        if props.get('node.name') == OWN_NODE_NAME:
            return True
        client = self.objects.get(props.get('client.id'))
        if client is not None and own_pids:
            return self._props(client).get('pipewire.sec.pid') in own_pids
        return False

    def cameras(self, own_pids=frozenset()):
        cams = {}
        links = [o['info'] for o in self.objects.values()
                 if o.get('type') == 'PipeWire:Interface:Link']
        for obj in self.objects.values():
            if obj.get('type') != 'PipeWire:Interface:Node':
                continue
            props = self._props(obj)
            name = props.get('node.name', '')
            if (props.get('media.class') != 'Video/Source'
                    or not name.startswith('libcamera_input')):
                continue
            in_use = any(l.get('output-node-id') == obj['id']
                         and l.get('state') == 'active'
                         and not self._is_ours(l.get('input-node-id'), own_pids)
                         for l in links)
            cams[obj['id']] = CameraNode(obj['id'], props.get('object.serial'),
                                         name, sensor_kind(name), in_use)
        return cams
