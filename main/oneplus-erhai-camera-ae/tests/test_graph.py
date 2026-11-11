import json
import os
import unittest

from camera_ae.graph import Graph, sensor_kind

FRONT = 'libcamera_input._base_soc_0_geniqup_8c0000_i2c_884000_camera-sensor_10'
REAR = 'libcamera_input._base_soc_0_cci_ac7b000_i2c-bus_1_camera-sensor_36'
FIXTURE = os.path.join(os.path.dirname(__file__), 'fixtures', 'pwdump-idle.json')


def node(id, name, serial, client, cls='Video/Source'):
    return {'id': id, 'type': 'PipeWire:Interface:Node',
            'info': {'props': {'node.name': name, 'object.serial': serial,
                               'media.class': cls, 'client.id': client}}}


def client(id, pid, name='app'):
    return {'id': id, 'type': 'PipeWire:Interface:Client',
            'info': {'props': {'application.name': name,
                               'pipewire.sec.pid': pid}}}


def link(id, out, inp, state='active'):
    return {'id': id, 'type': 'PipeWire:Interface:Link',
            'info': {'output-node-id': out, 'input-node-id': inp,
                     'state': state}}


BASE = [client(39, 8508, 'WirePlumber [export]'), client(50, 4000, 'zen'),
        client(51, 4100, 'gst-launch-1.0'),
        node(33, FRONT, 82, 39), node(76, REAR, 84, 39),
        node(90, 'zen-capture', 300, 50, 'Stream/Input/Video'),
        node(91, 'camera-tone', 301, 51, 'Stream/Input/Video')]


def dump(objs):
    return json.dumps(objs, indent=2).encode() + b'\n'


class GraphTests(unittest.TestCase):
    def test_graph_deltas(self):
        g = Graph()
        g.feed(dump(BASE))
        self.assertFalse(g.cameras()[33].in_use)
        g.feed(dump([link(200, 33, 90)]))
        self.assertTrue(g.cameras()[33].in_use)
        g.feed(dump([{'id': 200, 'info': None}]))
        self.assertFalse(g.cameras()[33].in_use)
        g2 = Graph()
        g2.feed(dump(BASE) + dump([link(201, 33, 91)]))
        cams = g2.cameras()
        self.assertEqual(set(cams), {33, 76})
        self.assertFalse(cams[33].in_use)          # only our own reader

    def test_own_pid_link_not_in_use(self):
        g = Graph()
        g.feed(dump(BASE + [link(202, 76, 90)]))
        self.assertTrue(g.cameras()[76].in_use)
        self.assertFalse(g.cameras(own_pids={4000})[76].in_use)

    def test_graph_split_chunk(self):
        data = dump(BASE + [link(200, 33, 90)])
        g = Graph()
        for i in range(0, len(data), 37):
            g.feed(data[i:i + 37])
        self.assertTrue(g.cameras()[33].in_use)

    def test_graph_utf8_split(self):
        objs = BASE + [node(92, 'caméra-zoë', 302, 50, 'Stream/Input/Video'),
                       link(200, 33, 92)]
        data = json.dumps(objs, ensure_ascii=False).encode()
        cut = data.index('é'.encode()) + 1          # split inside 'é'
        g = Graph()
        g.feed(data[:cut])
        g.feed(data[cut:])
        self.assertTrue(g.cameras()[33].in_use)

    def test_link_paused_not_in_use(self):
        g = Graph()
        g.feed(dump(BASE + [link(200, 33, 90, state='paused')]))
        self.assertFalse(g.cameras()[33].in_use)

    def test_fixture_parses(self):
        g = Graph()
        with open(FIXTURE, 'rb') as f:
            g.feed(f.read())
        cams = g.cameras()
        kinds = {c.kind: c for c in cams.values()}
        self.assertEqual(set(kinds), {'front', 'rear'})
        self.assertEqual((kinds['front'].id, kinds['front'].serial), (33, 82))
        self.assertTrue(kinds['rear'].name.endswith('camera-sensor_36'))

    def test_sensor_kind(self):
        self.assertEqual(sensor_kind(FRONT), 'front')
        self.assertEqual(sensor_kind(REAR), 'rear')
        self.assertIsNone(sensor_kind('libcamera_input.something_else'))


if __name__ == '__main__':
    unittest.main()
