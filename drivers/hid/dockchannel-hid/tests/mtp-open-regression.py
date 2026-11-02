#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise production HID open/error paths with injected transport failures."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--revision', help='Test source from a Git revision')
parser.add_argument('--case', choices=['all', 'magicmouse', 'transport'], default='all')
args = parser.parse_args()

def source(name):
    if args.revision:
        return subprocess.check_output(['git', '-C', str(ROOT), 'show', f'{args.revision}:{name}'], text=True)
    return (ROOT / name).read_text()

def function(text, name):
    start = text.index('static int ' + name + '(')
    body = text.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

common = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t __le16;
typedef uint32_t __le32;
#define le32_to_cpu(x) (x)
static int le16_to_int(__le16 x) { return (int16_t)x; }
#define hid_err(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_info(...) ((void)0)
#define msecs_to_jiffies(x) (x)
'''
magic = common + r'''
#define BUS_HOST 1
#define USB_DEVICE_ID_APPLE_MAGICMOUSE2 2
#define USB_DEVICE_ID_APPLE_MAGICMOUSE2_USBC 3
#define SENSOR_DIMENSIONS_REPORT_ID 0xd9
#define HID_FEATURE_REPORT 0
#define HID_REQ_GET_REPORT 0
#define ABS_MT_POSITION_X 0
#define ABS_MT_POSITION_Y 1
#define ABS_X 2
#define ABS_Y 3
struct input_absinfo { int min, max, res; };
struct input_dev { void *driver; struct input_absinfo absinfo[4]; };
struct magicmouse_sc { bool query_dimensions; struct input_dev *input; int work; };
struct hid_device { int bus, product; struct magicmouse_sc *data; };
static int refs, closes, open_error, enable_error, response;
static struct { __le32 width, height; __le16 min_x, min_y, max_x, max_y; } wire;
static void *input_get_drvdata(struct input_dev *d) { return d->driver; }
static void *hid_get_drvdata(struct hid_device *d) { return d->data; }
static int hid_hw_open(struct hid_device *d) { if (open_error) return open_error; refs++; return 0; }
static void hid_hw_close(struct hid_device *d) { refs--; closes++; }
static int magicmouse_enable_multitouch(struct hid_device *d) { return enable_error; }
static void schedule_delayed_work(void *p, int timeout) {}
static int hid_hw_raw_request(struct hid_device *d, int id, void *buf, size_t size, int type, int req) {
    memcpy((char *)buf+1, &wire, sizeof(wire)); return response;
}
static void input_set_abs_params(struct input_dev *d, int axis, int min, int max, int fuzz, int flat) {
    d->absinfo[axis].min=min; d->absinfo[axis].max=max;
}
static void input_abs_set_res(struct input_dev *d, int axis, int value) { d->absinfo[axis].res=value; }
''' + function(source('drivers/hid/hid-magicmouse.c'), 'magicmouse_open') + r'''
static struct input_dev input;
static struct magicmouse_sc msc;
static struct hid_device hid;
static void reset(void) {
    refs=closes=open_error=enable_error=0; response=17;
    wire.width=11200; wire.height=6800;
    wire.min_x=(uint16_t)-5500; wire.max_x=5500;
    wire.min_y=(uint16_t)-6500; wire.max_y=100;
    memset(&input,0,sizeof(input));
    msc=(struct magicmouse_sc){ .query_dimensions=true, .input=&input };
    hid=(struct hid_device){ .bus=BUS_HOST, .data=&msc }; input.driver=&hid;
}
int main(void) {
    reset(); open_error=-ENODEV;
    assert(magicmouse_open(&input)==-ENODEV && refs==0 && closes==0);
    reset(); hid.bus=0; enable_error=-ETIMEDOUT;
    assert(magicmouse_open(&input)==-ETIMEDOUT && refs==0 && closes==1);
    for (int value=-EIO; value<17; value++) {
        reset(); response=value;
        int result=magicmouse_open(&input);
        assert(result==(value<0 ? value : -EIO));
        assert(refs==0 && closes==1 && msc.query_dimensions);
    }
    reset(); wire.width=0;
    assert(magicmouse_open(&input)==-EINVAL && refs==0 && closes==1);
    reset(); wire.height=0;
    assert(magicmouse_open(&input)==-EINVAL && refs==0 && closes==1);
    reset(); wire.max_x=wire.min_x;
    assert(magicmouse_open(&input)==-EINVAL && refs==0 && closes==1);
    reset(); response=-ETIMEDOUT;
    assert(magicmouse_open(&input)==-ETIMEDOUT && refs==0);
    response=17;
    assert(magicmouse_open(&input)==0 && refs==1 && !msc.query_dimensions);
    assert(input.absinfo[ABS_X].res==98 && input.absinfo[ABS_Y].res==97);
    hid_hw_close(&hid); assert(refs==0);
}
'''
transport = common + r'''
#define START_TIMEOUT_MS 1
struct dockchannel_hid { void *dev; bool use_ring; };
struct dchid_iface { bool starting, open; int ready, gpio_id; const char *name; struct dockchannel_hid *dchid; };
struct hid_device { struct dchid_iface *driver_data; };
static int prepare_calls, reset_zero, reset_two, reset_two_calls;
static bool ready_on_wait;
static int dchid_register_ring(struct dockchannel_hid *d) { prepare_calls++; return 0; }
static int dchid_get_firmware(struct dchid_iface *i, void **fw, size_t *size) { static int data; *fw=&data; *size=4; return 0; }
static int dchid_request_gpio(struct dchid_iface *i) { return 0; }
static int dchid_send_firmware(struct dchid_iface *i, void *fw, size_t size) { return 0; }
static int dchid_reset_interface(struct dchid_iface *i, int state) {
    if (!state) return reset_zero;
    reset_two_calls++; return reset_two;
}
static bool completion_done(int *ready) { return *ready; }
static int wait_for_completion_timeout(int *ready, int timeout) { if (ready_on_wait) *ready=1; return *ready; }
''' + function(source('drivers/hid/dockchannel-hid/dockchannel-hid.c'), 'dchid_start_interface') + '\n' + function(source('drivers/hid/dockchannel-hid/dockchannel-hid.c'), 'dchid_open') + r'''
int main(void) {
    struct dockchannel_hid dc={0};
    struct dchid_iface iface={ .name="multi-touch", .dchid=&dc };
    struct hid_device h={ .driver_data=&iface };
    reset_zero=-EIO;
    assert(dchid_open(&h)==-EIO && !iface.starting && !iface.open && !reset_two_calls);
    reset_zero=0; reset_two=-EPROTO;
    assert(dchid_open(&h)==-EPROTO && !iface.starting && !iface.open);
    reset_two=0; prepare_calls=0; ready_on_wait=false;
    assert(dchid_open(&h)==-ETIMEDOUT && iface.starting && !iface.open);
    assert(prepare_calls==1);
    ready_on_wait=true;
    assert(dchid_open(&h)==0 && iface.open && prepare_calls==1);
}
'''
with tempfile.TemporaryDirectory() as directory:
    d=Path(directory)
    for case, name, text in [('magicmouse','magicmouse-open',magic),('transport','dockchannel-open',transport)]:
        if args.case not in ('all', case):
            continue
        c=d/(name+'.c'); exe=d/name
        c.write_text(text)
        subprocess.run(['cc','-std=c11','-Wall','-Werror','-Wno-unused-parameter','-Wno-unused-function',str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
        print('PASS:',name,flush=True)
