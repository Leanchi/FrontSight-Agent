/*
Copyright 2020 Intel Corporation

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/


const MB_OK                     = 0x00000000;
const MB_OKCANCEL               = 0x00000001;
const MB_ABORTRETRYIGNORE       = 0x00000002;
const MB_YESNOCANCEL            = 0x00000003;
const MB_YESNO                  = 0x00000004;
const MB_RETRYCANCEL            = 0x00000005;
const MB_TOPMOST                = 0x00040000;
const MB_SETFOREGROUND          = 0x00010000;
const MB_SYSTEMMODAL            = 0x00001000;

const MB_DEFBUTTON1             = 0x00000000;
const MB_DEFBUTTON2             = 0x00000100;
const MB_DEFBUTTON3             = 0x00000200;
const MB_ICONHAND               = 0x00000010;
const MB_ICONQUESTION           = 0x00000020;
const MB_ICONEXCLAMATION        = 0x00000030;
const MB_ICONASTERISK           = 0x00000040;

const IDOK     = 1;
const IDCANCEL = 2;
const IDABORT  = 3;
const IDRETRY  = 4;
const IDIGNORE = 5;
const IDYES    = 6;
const IDNO     = 7;
const WM_CLOSE = 0x0010;

var promise = require('promise');

function sendConsoleText(msg)
{
    require('MeshAgent').SendCommand({ action: 'msg', type: 'console', value: msg });
}


function messageBox()
{
    this._ObjectID = 'message-box';
    this.create = function create(title, caption, timeout, layout, sid)
    {
        if (title == 'MeshCentral') { try { title = require('MeshAgent').displayName; } catch (x) { } }
        var ret = new promise(function (res, rej) { this._res = res; this._rej = rej; });
        ret.options = { launch: { module: 'message-box', method: 'slave', args: [] } };
        ret.title = title;
        ret.caption = caption;
        ret.timeout = timeout;
        ret.layout = layout;

        //ret.options._debugIPC = true;
        //ret.options._ipcInteger = 1500;

        try
        {
            ret.options.uid = sid == null ? require('user-sessions').consoleUid() : sid;
            if (ret.options.uid == require('user-sessions').getProcessOwnerName(process.pid).tsid) { delete ret.options.uid; }
            if (sid == null && require('user-sessions').locked()) { ret.options.uid = -1; }
        }
        catch (ee)
        {
            if (sid == null)
            {
                ret.options.uid = -1;
            }
            else
            {
                ret._rej(ee);
                return (ret);
            }
        }

        ret._ipc = require('child-container').create(ret.options);
        ret._ipc.master = ret;
        ret._ipc.on('ready', function ()
        {
            this.descriptorMetadata = 'message-box';
            if (this.master.timeout != null) { this.master._timeout = setTimeout(function (mstr) { mstr._ipc.exit(); }, this.master.timeout * 1000, this.master); }
            if (this.master.layout == null)
            {
                this.message({ command: 'YESNO', caption: this.master.caption, title: this.master.title });
            }
            else
            {
                this.message({ command: 'ALERT', caption: this.master.caption, title: this.master.title });
            }
        });
        ret._ipc.on('message', function (msg)
        {
            try
            {
                switch(msg.command)
                {
                    case 'response':
                        if (this.master._timeout) { clearTimeout(this.master._timeout); this.master._timeout = null; }
                        if (msg.response == IDYES || msg.response == IDOK)
                        {
                            this.master._res();
                        }
                        else
                        {
                            this.master._rej(msg.response);
                        }
                        break;
                    default:
                        break;
                }
            }
            catch(ff)
            {
            }
        });
        ret._ipc.on('exit', function (c) { this.master._rej('child exited with code: ' + c); });
        ret.close = function close()
        {
            ret._ipc.exit();
        };
        return (ret);
    };
    this.slave = function()
    {
        var master = require('child-container');
        master.on('message', function (msg)
        {
            switch(msg.command)
            {
                case 'YESNO':
                case 'ALERT':
                    this.GM = require('_GenericMarshal');
                    this.user32 = this.GM.CreateNativeProxy('user32.dll');
                    this.user32.CreateMethod('MessageBoxW');

                    layout = msg.command == 'YESNO' ? (MB_YESNO | MB_DEFBUTTON2 | MB_ICONEXCLAMATION | MB_TOPMOST | MB_SYSTEMMODAL) : (MB_OK | MB_DEFBUTTON2 | MB_ICONEXCLAMATION | MB_TOPMOST | MB_SYSTEMMODAL);
                    this.user32.MessageBoxW.async(0, this.GM.CreateVariable(msg.caption, { wide: true }), this.GM.CreateVariable(msg.title, { wide: true }), layout)
                        .then(function (r)
                        {
                            try
                            {
                                switch(r.Val)
                                {
                                    case IDOK:
                                    case IDCANCEL:
                                    case IDABORT:
                                    case IDRETRY:
                                    case IDIGNORE:
                                    case IDYES:
                                        this.that.message({command: 'response', response: r.Val});
                                        break;
                                    default:
                                        this.that.message({command: 'response', response: IDNO});
                                        break;
                                }
                            }
                            catch(ff)
                            {
                            }
                            process.exit();
                        }, function () { process.exit(); }).parentPromise.that = this;
                    break;
                default:
                    break;
            }
        });
    }
}

function linux_messageBox()
{
    this._ObjectID = 'message-box';
    Object.defineProperty(this, 'zenity',
        {
            value: (function ()
            {
                var child = require('child_process').execFile('/bin/sh', ['sh']);
                child.stdout.str = ''; child.stdout.on('data', function (chunk) { this.str += chunk.toString(); });
                child.stdin.write("whereis zenity | awk '{ print $2 }'\nexit\n");
                child.waitExit();
                var location = child.stdout.str.trim();
                if (location.split('/man/').length > 1) { location = ''; }
                if (location == '' && require('fs').existsSync('/usr/local/bin/zenity')) { location = '/usr/local/bin/zenity'; }
                if (location == '') { return (null); }

                var ret = { path: location, timeout: child.stdout.str.trim() == '' ? false : true };
                Object.defineProperty(ret, "timeout", {
                    get: function ()
                    {
                        if (this._timeout == null)
                        {
                            var uid, xinfo;
                            try
                            {
                                uid = require('user-sessions').consoleUid();
                                xinfo = require('monitor-info').getXInfo(uid);
                            }
                            catch (e)
                            {
                                uid = 0;
                                xinfo = require('monitor-info').getXInfo(0);
                            }
                            if (xinfo == null) { return (false); }
                            var child = require('child_process').execFile('/bin/sh', ['sh'], { uid: uid, env: { XAUTHORITY: xinfo.xauthority ? xinfo.xauthority : "", DISPLAY: xinfo.display } });
                            child.stdout.str = ''; child.stdout.on('data', function (chunk) { this.str += chunk.toString(); });
                            child.stdin.write(location + ' --help-all | grep timeout\nexit\n');
                            child.stderr.on('data', function (e) { });
                            child.waitExit();
                            Object.defineProperty(this, "_timeout", { value: child.stdout.str.trim() == '' ? false : true });
                            return (this._timeout);
                        }
                        else
                        {
                            return (this._timeout);
                        }
                    }
                });
                Object.defineProperty(ret, "extra", {
                    get: function ()
                    {
                        if (this._extra == null)
                        {
                            var uid, xinfo;
                            try
                            {
                                uid = require('user-sessions').consoleUid();
                                xinfo = require('monitor-info').getXInfo(uid);
                            }
                            catch (e)
                            {
                                uid = 0;
                                xinfo = require('monitor-info').getXInfo(0);
                            }
                            if (xinfo == null) { return (false); }
                            var child = require('child_process').execFile('/bin/sh', ['sh'], { uid: uid, env: { XAUTHORITY: xinfo.xauthority ? xinfo.xauthority : "", DISPLAY: xinfo.display } });
                            child.stdout.str = ''; child.stdout.on('data', function (chunk) { this.str += chunk.toString(); });
                            child.stdin.write(location + ' --help-all | grep extra-button\nexit\n');
                            child.stderr.on('data', function (e) { });
                            child.waitExit();
                            Object.defineProperty(this, "_extra", { value: child.stdout.str.trim() == '' ? false : true });
                            return (this._extra);
                        }
                        else
                        {
                            return (this._extra);
                        }
                    }
                });
                Object.defineProperty(ret, "version", {
                    get: function ()
                    {
                        if (this._version == null)
                        {
                            var uid, xinfo;
                            try
                            {
                                uid = require('user-sessions').consoleUid();
                                xinfo = require('monitor-info').getXInfo(uid);
                            }
                            catch (e)
                            {
                                uid = 0;
                                xinfo = require('monitor-info').getXInfo(0);
                            }
                            if (xinfo == null) { return (false); }

                            var child = require('child_process').execFile('/bin/sh', ['sh'], { uid: uid, env: { XAUTHORITY: xinfo.xauthority ? xinfo.xauthority : "", DISPLAY: xinfo.display } });
                            child.stdout.str = ''; child.stdout.on('data', function (chunk) { this.str += chunk.toString(); });
                            child.stderr.str = ''; child.stderr.on('data', function (chunk) { this.str += chunk.toString(); });
                            child.stdin.write(location + ' --version | awk -F. \'{ printf "[%s, %s]\\n", $1, $2; } \'\nexit\n');
                            child.waitExit();

                            try
                            {
                                if (child.stderr.str.includes('-CRITICAL **')) { object.defineProperty(this, "broken", { value: true }); }
                                Object.defineProperty(this, "_version", {value: JSON.parse(child.stdout.str.trim())});
                                return (this._version);
                            }
                            catch (e)
                            {
                                Object.defineProperty(this, "_version", { value: [2, 16] });
                                return (this._version);
                            }
                        }
                        else
                        {
                            return (this._version);
                        }
                    }
                });
                return (ret);
            })()
        });
    if (!this.zenity)
    {
        Object.defineProperty(this, 'kdialog',
            {
                value: (function ()
                {
                    var child = require('child_process').execFile('/bin/sh', ['sh']);
                    child.stdout.str = ''; child.stdout.on('data', function (chunk) { this.str += chunk.toString(); });
                    child.stdin.write("whereis kdialog | awk '{ print $2 }'\nexit\n");
                    child.waitExit();
                    return (child.stdout.str.trim() == '' ? null : { path: child.stdout.str.trim() });
                })()
            });
        Object.defineProperty(this, 'xmessage',
            {
                value: (function ()
                {
                    var child = require('child_process').execFile('/bin/sh', ['sh']);
                    child.stdout.str = ''; child.stdout.on('data', function (chunk) { this.str += chunk.toString(); });
                    child.stdin.write("whereis xmessage | awk '{ print $2 }'\nexit\n");
                    child.waitExit();
                    return (child.stdout.str.trim() == '' ? null : { path: child.stdout.str.trim() });
                })()
            });
    }

    Object.defineProperty(this, 'notifysend',
        {
            value: (function ()
            {
                var child = require('child_process').execFile('/bin/sh', ['sh']);
                child.stdout.str = ''; child.stdout.on('data', function (chunk) { this.str += chunk.toString(); });
                child.stdin.write("whereis notify-send | awk '{ print $2 }'\nexit\n");
                child.waitExit();
                return (child.stdout.str.trim() == '' ? null : { path: child.stdout.str.trim() });
            })()
        });
    

    this.create = function create(title, caption, timeout, layout)
    {
        if (timeout == null) { timeout = 10; }
        if (title == 'MeshCentral') { try { title = require('MeshAgent').displayName; } catch (x) { } }
        var ret = new promise(function (res, rej) { this._res = res; this._rej = rej; });
        var uid;    
        var xinfo;
        var min = require('user-sessions').minUid();

        try
        {
            uid = require('user-sessions').consoleUid();
            xinfo = require('monitor-info').getXInfo(uid);
        }
        catch(e)
        {
            uid = 0;
            xinfo = require('monitor-info').getXInfo(0);
        }

        if (xinfo == null || (uid != 0 && uid < min))
        {
            ret._rej('This system cannot display a user dialog box when a user is not logged in');
            return (ret);
        }
        if (this.zenity)
        {
            if (!this.zenity.extra && Array.isArray(layout) && layout.length > 1)
            {
                ret._rej('This system does not support custom button layouts');
                return (ret);
            }
            // GNOME/ZENITY
            ret._options = { title: title.trim().split('').join('\\'), caption: caption.trim().split('').join('\\'), timeout: timeout, layout: layout, zenity: this.zenity };
            var parms = ['zenity'];
            if (Array.isArray(layout))
            {
                var i;
                parms.push('--info');
                for(i=0;i<layout.length;++i)
                {
                    if(i==0)
                    {
                        parms.push('--ok-label=' + layout[i]);
                    }
                    else
                    {
                        parms.push('--extra-button=' + layout[i]);
                    }
                }
            }
            else
            {
                parms.push(layout == null ? '--question' : '--warning');
            }
            parms.push('--title=' + title);
            parms.push('--text=' + caption);
            parms.push('--no-wrap');
            if (this.zenity.timeout) { parms.push('--timeout=' + timeout); }
            try { MeshServerLogEx(200, [], 'message-box: zenity exec, uid=' + uid + ', path=' + this.zenity.path, null); } catch (e) {}
            ret.child = require('child_process').execFile(this.zenity.path, parms, { uid: uid, env: { XAUTHORITY: xinfo.xauthority ? xinfo.xauthority : "", DISPLAY: xinfo.display } });
            if (this.zenity.timeout)
            {
                ret.child.timeout = setTimeout(function (c)
                {
                    c.timeout = null;
                    c.promise._rej('timeout');
                    c.kill();
                }, timeout * 1000, ret.child);
            }

            ret.child.descriptorMetadata = 'zenity, message-box'
            ret.child.promise = ret;
            ret.child.stderr.str = ''; ret.child.stderr.on('data', function (chunk) { this.str += chunk.toString(); });
            ret.child.stdout.ostr = ''; ret.child.stdout.on('data', function (chunk) { this.ostr += chunk.toString(); });
            ret.child.on('exit', function (code)
            {
                if (this.timeout) { clearTimeout(this.timeout); }
                if (!(this.stderr.str.includes('option is not') && this.promise._options.zenity.timeout))
                {
                    if (Array.isArray(this.promise._options.layout))
                    {
                        if (code == 0 && ((process.platform == 'freebsd' && this.stdout.ostr.trim() == '') || process.platform != 'freebsd'))
                        {
                            this.promise._res(this.promise._options.layout[0]);
                        }
                        else
                        {
                            var val = this.stdout.ostr.trim();
                            for (var i = 1; i < this.promise._options.layout.length; ++i)
                            {
                                if (this.promise._options.layout[i] == val)
                                {
                                    this.promise._res(val);
                                    return;
                                }
                            }
                            this.promise._rej('timeout');
                        }
                    }
                }
                switch (code)
                {
                    case 0:
                        this.promise._res();
                        break;
                    case 1:
                        this.promise._rej('denied');
                        break;
                    default:
                        if (this.stderr.str.includes('option is not') && this.promise._options.zenity.timeout)
                        {
                            var uname = require('user-sessions').getUsername(uid);
                            this.promise._ch = require('child_process').execFile('/bin/sh', ['sh'], { type: require('child_process').SpawnTypes.TERM });
                            this.promise._ch.promise = this.promise;
                            this.promise.child = this.promise._ch;
                            this.promise._ch.stderr.str = ''; this.promise._ch.stderr.on('data', function (c) { this.str += c.toString(); });
                            this.promise._ch.stdout.str = ''; this.promise._ch.stdout.on('data', function (c)
                            {
                                this.str += c.toString();
                                if (Array.isArray(layout))
                                {
                                    var i;
                                    var tmp = c.toString().trim();
                                    for (i = 0; i < layout.length; ++i)
                                    {
                                        if(layout[i] == tmp)
                                        {
                                            this.parent.result = tmp;
                                            //this.parent.promise._res(tmp);
                                            return;
                                        }
                                    }
                                }
                                if (this.str.includes('<<<<$_RESULT>>>>')) { this.str = this.str.split('<<<<$_RESULT>>>>')[1]; }
                                if (this.str.includes('>>>>')) { this.parent.kill(); }
                            });
                            this.promise._ch.stdin.write('su - ' + uname + '\n');
                            this.promise._ch.stdin.write('export DISPLAY=' + xinfo.display + '\n');
                            this.promise._ch.stdin.write('zenity ');
                            if (Array.isArray(layout))
                            {
                                var i;
                                this.promise._ch.stdin.write('--info');
                                for (i = 0; i < layout.length; ++i)
                                {
                                    if (i == 0)
                                    {
                                        this.promise._ch.stdin.write(' --ok-label=' + layout[i]);
                                    }
                                    else
                                    {
                                        this.promise._ch.stdin.write(' --extra-button=' + layout[i]);
                                    }
                                }
                            }
                            else
                            {
                                this.promise._ch.stdin.write((this.promise._options.layout == null ? '--question' : '--warning'));
                            }
                            this.promise._ch.stdin.write(' --title=' + this.promise._options.title + ' --text=' + this.promise._options.caption);
                            this.promise._ch.stdin.write(' --timeout=' + this.promise._options.timeout + '\nexport _RESULT=$?\necho "<<<<$_RESULT>>>>"\nexit');
                            this.promise._ch.on('exit', function ()
                            {
                                if (this.result != null)
                                {
                                    this.promise._res(this.result);
                                    return;
                                }
                                var res = this.stdout.str.split('>>>>')[0].split('<<<<')[1];
                                switch(parseInt(res))
                                {
                                    case 0:
                                        this.promise._res();
                                        break;
                                    case 1:
                                        this.promise._rej('denied');
                                        break;
                                    default:
                                        this.promise._rej(this.stderr.str.toString());
                                        break;
                                }
                            });

                        }
                        else
                        {
                            this.promise._rej(this.stderr.str.trim());
                        }
                        break;
                }
            });
        }
        else if(this.kdialog)
        {
            var msgparms = ['kdialog', '--title', title];
            if (Array.isArray(layout))
            {
                if (layout.length > 3) { ret._rej('KDialog only supports up to 3 button layouts'); return (ret); }
                ret.user = true;
                switch(layout.length)
                {
                    case 0:
                    case 1:
                        msgparms.push('--msgbox');
                        break;
                    case 2:
                        msgparms.push('--yesno');
                        break;
                    case 3:
                        msgparms.push('--yesnocancel');
                        break;
                }
                msgparms.push(caption);

                for (var i = 0; i < layout.length; ++i)
                {
                    switch(i)
                    {
                        case 0:
                            msgparms.push('--yes-label=' + layout[i]);
                            break;
                        case 1:
                            msgparms.push('--no-label=' + layout[i]);
                            break;
                        case 2:
                            msgparms.push('--cancel-label=' + layout[i]);
                            break;
                    }
                }
            }
            else
            {
                msgparms.push(layout == null ? '--yesno' : '--msgbox');
                msgparms.push(caption);
            }

            if (process.platform != 'freebsd' && process.env['DISPLAY'])
            {
                ret.child = require('child_process').execFile(this.kdialog.path, msgparms);
                ret.child.promise = ret;
            }
            else
            {
                var xdg = require('user-sessions').findEnv(uid, 'XDG_RUNTIME_DIR'); if (xdg == null) { xdg = ''; }
                if (!xinfo || !xinfo.display || !xinfo.xauthority) { ret._rej('Interal Error, could not determine X11/XDG env'); return (ret); }
                try { MeshServerLogEx(200, [], 'message-box: kdialog exec, uid=' + uid + ', path=' + this.kdialog.path, null); } catch (e) {}
                ret.child = require('child_process').execFile(this.kdialog.path, msgparms, { uid: uid, env: { DISPLAY: xinfo.display, XAUTHORITY: xinfo.xauthority, XDG_RUNTIME_DIR: xdg } });
                ret.child.promise = ret;
            }
            ret.child.descriptorMetadata = 'kdialog, message-box'
            ret.child.timeout = setTimeout(function (c)
            {
                c.timeout = null;
                c.kill();
            }, timeout * 1000, ret.child);
            ret.child.stdout.on('data', function (chunk) { });
            ret.child.stderr.on('data', function (chunk) { });
            ret.child.buttons = layout;
            ret.child.on('exit', function (code)
            {
                if (this.timeout)
                {
                    clearTimeout(this.timeout);
                    if (this.promise.user)
                    {
                        this.promise._res(this.buttons[code]);
                    }
                    else
                    {
                        switch (code)
                        {
                            case 0:
                                this.promise._res();
                                break;
                            case 1:
                                this.promise._rej('denied');
                                break;
                            default:
                                this.promise._rej('timeout');
                                break;
                        }
                    }
                }
                else
                {
                    this.promise._rej('timeout');
                }
            });
        }
        else if (this.xmessage)
        {
            // title, caption, timeout, layout
            try { MeshServerLogEx(200, [], 'message-box: xmessage exec, uid=' + uid + ', path=' + this.xmessage.path, null); } catch (e) {}
            ret.child = require('child_process').execFile(this.xmessage.path, ['xmessage', '-center', '-buttons', layout == null ? 'No:1,Yes:2' : 'OK:2', '-timeout', timeout.toString(), '-default', layout==null?'No':'OK', '-title', title, caption], { uid: uid, env: { XAUTHORITY: xinfo.xauthority ? xinfo.xauthority : "", DISPLAY: xinfo.display } });
            ret.child.stdout.on('data', function (c) {  });
            ret.child.stderr.on('data', function (c) {  });
            ret.child.descriptorMetadata = 'xmessage, message-box'
            ret.child.promise = ret;
            ret.child.on('exit', function (code)
            {
                switch(code)
                {
                    case 2:
                        this.promise._res();
                        break;
                    case 1:
                        this.promise._rej('denied');
                        break;
                    default:
                        this.promise._rej('timeout');
                        break;
                }
            });
        }
        else
        {
            ret._rej('Unable to create dialog box');
        }

        ret.close = function close()
        {
            if (this.timeout) { clearTimeout(this.timeout); }
            if (this.child)
            {
                this._rej('denied');
                this.child.kill();
            }
        }
        return (ret);
    };
}

if (process.platform == 'darwin')
{
    function translateObject(obj)
    {
        var j = JSON.stringify(obj);
        var b = Buffer.alloc(j.length + 4);
        b.writeUInt32LE(j.length + 4);
        Buffer.from(j).copy(b, 4);
        return (b);
    }

    // Decodes the length-prefixed framing used on the message-box IPC sockets:
    //   [uint32 LE total frame length (including the 4 byte header)][JSON payload]
    // Hardening (2026-09-18):
    //   * Buffers partial frames across multiple 'data' chunks (the old code used buffer.len,
    //     which is undefined on Duktape Buffer, so every length check was a no-op and
    //     truncated/garbage chunks went straight into JSON.parse)
    //   * Splits multiple frames coalesced into a single chunk
    //   * Parse failures are routed to onProtocolError instead of throwing out of the
    //     'data' handler (which surfaced as UncaughtException: invalid json)
    // onFrame returning false stops processing (connection is being torn down).
    function decodeFrames(sock, buffer, onFrame, onProtocolError)
    {
        try
        {
            if (sock._mbrx != null && sock._mbrx.length > 0)
            {
                buffer = Buffer.concat([sock._mbrx, buffer]);
            }
            sock._mbrx = null;
            while (buffer.length >= 4)
            {
                var flen = buffer.readUInt32LE(0);
                if (flen < 4 || flen > 1048576) { onProtocolError(sock, 'invalid frame length'); return; }
                if (buffer.length < flen) { break; }
                var p = null;
                // [2026-09-20 leanchiliu] 终极修复 v2：真机字节级探针实证 IPC socket 外部缓冲的
                // 字符串转换路径损坏（Buffer.from(d).toString() 返回 null；slice().toString()
                // 可能返回乱码——两者都使 JSON.parse 失败→"invalid json"丢弃，是 consent 弹窗
                // 全链路秒断/静默乒乓的共同根因）。索引读/readUInt32LE 完好。策略：先试常规
                // toString+JSON.parse，失败即逐字节重建（fromCharCode latin-1）+ escape/decode
                // 还原 UTF-8（不可用则保留 latin-1，JSON 结构与 ASCII 键名仍完好）再解析。
                try { p = JSON.parse(buffer.slice(4, flen).toString()); }
                catch (e)
                {
                    var __str = '';
                    for (var ci = 4; ci < flen; ci++) { __str += String.fromCharCode(buffer[ci]); }
                    try { __str = decodeURIComponent(escape(__str)); } catch (eU) { }
                    try { p = JSON.parse(__str); }
                    catch (e2) { onProtocolError(sock, 'invalid json'); return; }
                }
                if (p == null || typeof p != 'object') { onProtocolError(sock, 'invalid json'); return; }
                if (onFrame(sock, p) === false) { return; }
                buffer = buffer.slice(flen);
            }
            // NOTE: The 'data' event buffer is backed by memory that is freed once the handler
            // returns (ILibDuktape_readableStream frees the backing allocation right after emit),
            // so a partial frame must be retained as a private copy. Buffer.concat() always
            // allocates a fresh buffer and copies, which is exactly what we need here.
            sock._mbrx = (buffer.length > 0 ? Buffer.concat([buffer]) : null);
        }
        catch (e)
        {
            try { sock._mbrx = null; } catch (e0) { }
            try { onProtocolError(sock, 'decoder error'); } catch (e2) { }
        }
    }
}

function macos_messageBox()
{
    this._ObjectID = 'message-box';
    this._initIPCBase = function _initIPCBase()
    {
        var ret = new promise(function (res, rej) { this._res = res; this._rej = rej; });

        try
        {
            ret.uid = require('user-sessions').consoleUid();
        }
        catch (e)
        {
            ret._rej(e);
            return (ret);
        }

        ret.path = '/var/tmp/' + process.execPath.split('/').pop() + '_ev';
        var n;

        try
        {
            n = require('tls').generateRandomInteger('1', '99999');
        }
        catch (e)
        {
            n = 0;
        }
        while (require('fs').existsSync(ret.path + n))
        {
            try {
                n = require('tls').generateRandomInteger('1', '99999');
            }
            catch (e) {
                ++n;
            }
        }
        ret.path = ret.path + n;
        ret.tmpServiceName = 'meshNotificationServer' + n;
        return (ret);
    };
    
    this.create = function create(title, caption, timeout, layout)
    {
        if (title == 'MeshCentral') { try { title = require('MeshAgent').displayName; } catch (x) { } }
        var userLayout = Array.isArray(layout);
        caption = caption.split('\n').join('\\n');
        if (Array.isArray(layout) && layout.length > 3)
        {
            ret = new promise(function (res, rej) { this._res = res; this._rej = rej; });
            ret._rej('This system only supports a maximum of 3 buttons');
            return (ret);
        }

        // [2026-09-20 leanchiliu] 会话内非 root 直弹（免 sudo 方案，第 41c 项）：
        // agent 本身就在控制台会话（Self()==consoleUid()，用户手动启动的常见形态）时，
        // 仿 root 主路径直接 spawn sh+osascript——只是无需 su（本就是该用户 GUI 会话，
        // D3 实测弹窗可见）。零 IPC/零 LaunchAgent/零 helper，管道 stdout 的 toString()
        // 实测完好（坏的是 IPC socket 外部缓冲的 toString 桥接——上游边缘路径的框架
        // 层缺陷，见 2026-09-20 排障记录）。root 走下方原路径；非 root 且不在会话内
        //（如 SSH 启动）退回 IPC 路径（受上述框架缺陷所阻，待上游修复）。
        // 回退：删除本分支即回到 IPC-only 行为。
        var __selfSess = null;
        try { __selfSess = require('user-sessions').Self(); } catch (eSelf) { }
        if (__selfSess != null && __selfSess == require('user-sessions').consoleUid())
        {
            var pr = new promise(function (res, rej) { this._res = res; this._rej = rej; });
            try
            {
                if (layout == null) { layout = ['Yes', 'No']; }
                else if (typeof (layout) != 'object') { layout = ['OK']; }
                var __buttons = 'buttons ' + JSON.stringify(layout).replace('[', '{').replace(']', '}');
                __buttons += (' default button "' + layout[layout.length - 1] + '"');
                var __tmo = (' giving up after ' + timeout);
                var __icon = 'with icon caution';
                pr.child = require('child_process').execFile('/bin/sh', ['sh']);
                pr.child.descriptorMetadata = 'message-box/insession';
                pr.child.promise = pr;
                pr.child.userLayout = userLayout;
                pr.child.stdout.str = ''; pr.child.stdout.on('data', function (c) { this.str += c.toString(); });
                pr.child.on('exit', function ()
                {
                    var r = this.stdout.str.split('\x1e');
                    if (!r[1]) { this.promise._rej('denied'); return; }
                    r = r[1].trim();
                    if (r == '_TIMEOUT_') { this.promise._rej('TIMEOUT'); }
                    else if (this.userLayout || r == 'Yes' || r == 'OK') { this.promise._res(r); }
                    else { this.promise._rej('denied'); }
                    this.promise.child = null;
                });
                pr.close = function close() { if (this.child) { this.child.kill(); } };
                pr.child.stdin.write('osascript -e \'tell current application to display dialog "' + caption + '" with title "' + title + '" ' + __icon + ' ' + __buttons + __tmo + '\' | awk \'{ printf "\\x1e"; c=split($0, tokens, ","); split(tokens[1], val, ":"); if(c==1) { print val[2] } else { split(tokens[2], gu, ":"); if(gu[2]=="true") { print "_TIMEOUT_" } else { print val[2]  }  } printf "\\x1e"; }\'\nexit\n');
                return (pr);
            }
            catch (eDlg) { pr._rej(eDlg); return (pr); }
        }

        if (require('user-sessions').isRoot())
        {
            ret = new promise(function (res, rej) { this._res = res; this._rej = rej; });

            try
            {
                ret.uid = require('user-sessions').consoleUid();
                ret.name = require('user-sessions').getUsername(ret.uid);
            }
            catch (ff)
            {
                ret._rej('No users logged in');
                return (ret);
            }
            ret.user = userLayout;
            if (layout == null)
            {
                layout = ['Yes', 'No'];
            }
            else if (typeof (layout) != 'object')
            {
                layout = ['OK'];
            }

            var buttons = 'buttons ' + JSON.stringify(layout).replace('[', '{').replace(']', '}');
            buttons += (' default button "' + layout[layout.length - 1] + '"');
            timeout = (' giving up after ' + timeout);
            var icon = 'with icon caution';
            var str = 'tell current application to display dialog "' + caption + '" with title "' + title + '" ' + icon + ' ' + buttons + timeout;
            str = Buffer.from("console.log('" + str + "'); process.exit();").toString('base64');

            ret.child = require('child_process').execFile('/bin/zsh', ['zsh'], { type: require('child_process').SpawnTypes.TERM });
            ret.child.descriptorMetadata = 'message-box';
            ret.child.promise = ret;
            ret.child.stdout.str = ''; ret.child.stdout.on('data', function (c) { this.str += c.toString(); });
            ret.child.on('exit', function (code)
            {
                var res = this.stdout.str.split('\x1e');
                if (this.promise.user && !res[1]) { this.promise._res('Cancel'); }
                if (!res[1]) { return; }
                res = res[1].trim();
                if (res == '_TIMEOUT_')
                {
                    this.promise._rej('TIMEOUT');
                }
                else
                {
                    if(this.promise.user || res == 'Yes' || res == 'OK')
                    {
                        this.promise._res(res);
                    }
                    else
                    {
                        this.promise._rej('denied');
                    }
                }
                this.promise.child = null;
            });
            ret.child.stdin.write('su - ' + ret.name + '\n');
            ret.child.stdin.write(process.execPath.split('./').join('').split(' ').join('\\ ') + ' -b64exec ' + str + ' | osascript 2>/dev/null | awk \'{ printf "\\x1e"; c=split($0, tokens, ","); split(tokens[1], val, ":"); if(c==1) { print val[2] } else { split(tokens[2], gu, ":"); if(gu[2]=="true") { print "_TIMEOUT_" } else { print val[2]  }  } printf "\\x1e"; }\'\nexit\nexit\n');
            ret.close = function close()
            {
                if (this.child) { this.child.kill(); }
            };

            return (ret);
        }


        // Start Local Server
        var ret = this._initIPCBase();
        ret.metadata = 'message-box/create'
        ret.title = title; ret.caption = caption; ret.timeout = timeout;
        if (layout == null)
        {
            ret.layout = ['Yes', 'No'];
        }
        else if(!Array.isArray(layout))
        {
            ret.layout = ['OK'];
        }
        else
        {
            ret.layout = layout;
            Object.defineProperty(ret.layout, "user", { value: true });
        }
        ret.server = this.startMessageServer(ret);
        ret.server.ret = ret;
        ret.server.on('connection', function (c)
        {
            this._connection = c;
            c.promise = this.ret;
            c.on('end', function ()
            {
                // Helper closed the connection without delivering a complete frame
                // (e.g. killed mid-write). Settle the dialog instead of hanging; duplicate
                // settles are ignored by the promise implementation, so the normal
                // DIALOG/ERROR flow is unaffected.
                try { this.promise._rej('Connection closed'); } catch (e) { }
                try { this.promise.server.close(); } catch (e) { }
            });
            c.on('data', function (buffer)
            {
                decodeFrames(this, buffer, function (sock, p)
                {
                    switch (p.command)
                    {
                        case 'ERROR':
                            sock.promise._rej(p.reason);
                            break;
                        case 'HELLO':
                            // [2026-09-20 leanchiliu] 握手就绪此刻才写 DIALOG（客户端 data 通路已证明可用）
                            try
                            {
                                sock.write(translateObject({ command: 'DIALOG', title: sock.promise.title, caption: sock.promise.caption, icon: 'caution', buttons: sock.promise.layout, buttonDefault: sock.promise.layout[sock.promise.layout.length - 1], timeout: sock.promise.timeout }));
                            } catch (e) { }
                            return (true);
                        case 'DIALOG':
                            if (p.timeout)
                            {
                                sock.promise._rej('TIMEOUT');
                            }
                            else
                            {
                                if (p.button == 'Yes' || p.button == 'OK' || sock.promise.layout.user)
                                {
                                    sock.promise._res(p.button);
                                }
                                else
                                {
                                    sock.promise._rej('denied');
                                }
                            }
                            break;
                    }
                    sock.promise.server.close();
                    if (sock.promise.server._replyTimer != null) { try { clearTimeout(sock.promise.server._replyTimer); } catch (e) { } sock.promise.server._replyTimer = null; }
                    return (false);
                },
                function (sock, err)
                {
                    // [2026-09-20 leanchiliu] 坏帧容忍：不 reject 不 close——真机实证新连接
                    // 首帧可能被 C 层搞坏（backing 失效→解码失败），HELLO 每秒重发会送来
                    // 好帧；此处掐断会让重试永远没机会（'Connection closed' 死循环）。真正的
                    // 兜底是外层 IPC 窗口超时。
                    try { sendConsoleText('message-box: 容忍坏帧(' + err + ')，等待重试'); } catch (e) { }
                });
            });
            for (var x in this.ret.layout)
            {
                this.ret.layout[x] = '"' + this.ret.layout[x] + '"';
            }
            // [2026-09-20 leanchiliu] HELLO 握手（DIALOG 帧投递竞态终修）：不在
            // on('connection') 内直接/延迟写 DIALOG——真机实证回环 unix socket 下
            // 连接内同步 write 与延迟 write 均有帧丢失形态（helper 秒死 'Connection
            // closed'、data 到达但 backing 失效、data 永不到达，随竞态窗口摆动）。
            // 改为 helper 连上先发 HELLO（见 startClient connect 分支），服务端
            // decodeFrames 收到 HELLO 才写 DIALOG（case 'HELLO'）——收到 HELLO 即
            // 客户端 data 通路已被证明可用，投递确定性由应用层保证。
            // setClipboard/getClipboard/notify/lock 的 connection 内同步 write 存在
            // 同款竞态（非 root 剪贴板等路径），后续同法迁移握手。回退：恢复同步 c.write。
        });
        ret.close = function close()
        {
            if (this.server) { this.server.close(); }
        };
        return (ret);
    };
    this.setClipboard = function setClipboard(clipText)
    {
        if (require('user-sessions').isRoot())
        {
            ret = new promise(function (res, rej) { this._res = res; this._rej = rej; });
            try
            {
                ret.uid = require('user-sessions').consoleUid();
                ret.name = require('user-sessions').getUsername(ret.uid);
            }
            catch(ff)
            {
                ret._rej('No users logged in');
                return (ret);
            }

            var agent = process.execPath.split('./').join('').split(' ').join('\\ ');
            var str = Buffer.from('console.log("' + clipText + '");process.exit();').toString('base64');
            ret.child = require('child_process').execFile('/bin/zsh', ['zsh'], { type: require('child_process').SpawnTypes.TERM });
            ret.child.promise = ret;
            ret.child.stdout.on('data', function (c) { });
            ret.child.on('exit', function () { this.promise._res(); });
            ret.child.stdin.write('su - ' + ret.name + '\n');
            ret.child.stdin.write(agent + ' -b64exec ' + str + ' | LANG=en_US.UTF-8 pbcopy\nexit\nexit\n');
            return (ret);
        }

        // Start Local Server
        var ret = this._initIPCBase();
        ret.metadata = 'clipboard/set'
        ret.server = this.startMessageServer(ret);
        ret.server.ret = ret;
        ret.server.clipText = clipText;
        ret.server.on('connection', function (c)
        {
            this._connection = c;
            c.promise = this.ret;
            c.on('end', function ()
            {
                // Peer closed without delivering a complete frame (e.g. helper killed mid-write).
                // Settle the promise instead of leaving it dangling; duplicate settles are
                // ignored by the promise implementation, so the normal flow is unaffected.
                try { this.promise._rej('Connection closed'); } catch (e) { }
                try { this.promise.server.close(); } catch (e) { }
            });
            c.on('data', function (buffer)
            {
                decodeFrames(this, buffer, function (sock, p)
                {
                    switch (p.command)
                    {
                        case 'writeClip':
                            if (p.clipError)
                            {
                                sock.promise._rej(p.clipError);
                            }
                            else
                            {
                                sock.promise._res();
                            }
                            break;
                    }
                },
                function (sock, err)
                {
                    try { sock.promise._rej('Protocol Error: ' + err); } catch (e) { }
                    try { sock.promise.server.close(); } catch (e) { }
                });
            });
            c.write(translateObject({ command: 'writeClip', clipText: this.clipText }));
        });

        return (ret);
    };
    this.getClipboard = function getClipboard()
    {
        if (require('user-sessions').isRoot())
        {
            ret = new promise(function (res, rej) { this._res = res; this._rej = rej; });
            try
            {
                ret.uid = require('user-sessions').consoleUid();
                ret.name = require('user-sessions').getUsername(ret.uid);
            }
            catch(ff)
            {
                ret._rej('No users logged in');
                return (ret);
            }
            ret.child = require('child_process').execFile('/bin/zsh', ['zsh'], { type: require('child_process').SpawnTypes.TERM });
            ret.child.promise = ret;
            ret.child.stdout.str = ''; ret.child.stdout.on('data', function (c) { this.str += c.toString(); });
            ret.child.on('exit', function ()
            {
                var res = this.stdout.str.split('\x1e')[1];
                // if (res.length > 2) { res = res.substring(0, res.length - 2); }
                this.promise._res(res);
            });
            ret.child.stdin.write('su - ' + ret.name + '\n');
            ret.child.stdin.write("LANG=en_US.UTF-8 pbpaste | tr '\\n' '\\035' | awk -F'\\035' '");
            ret.child.stdin.write('{');
            ret.child.stdin.write('   printf "\\036";');
            ret.child.stdin.write('   for(i=1;i<=NF;++i)');
            ret.child.stdin.write('   {');
            ret.child.stdin.write('      printf "%s%s", (i==1?"":"\\n"), $i;')
            ret.child.stdin.write('   }');
            ret.child.stdin.write('   printf "\\036";');
            ret.child.stdin.write("}'");
            ret.child.stdin.write('\nexit\nexit\n');
            return (ret);
        }

        // Start Local Server
        var ret = this._initIPCBase();
        ret.metadata = 'clipboard/get'
        ret.server = this.startMessageServer(ret);
        ret.server.ret = ret;
        ret.server.on('connection', function (c)
        {
            this._connection = c;
            c.promise = this.ret;
            c.on('end', function ()
            {
                // Peer closed without delivering a complete frame (e.g. helper killed mid-write).
                // Settle the promise instead of leaving it dangling; duplicate settles are
                // ignored by the promise implementation, so the normal flow is unaffected.
                try { this.promise._rej('Connection closed'); } catch (e) { }
                try { this.promise.server.close(); } catch (e) { }
            });
            c.on('data', function (buffer)
            {
                decodeFrames(this, buffer, function (sock, p)
                {
                    switch (p.command)
                    {
                        case 'readClip':
                            if (p.clipError)
                            {
                                sock.promise._rej(p.clipError);
                            }
                            else
                            {
                                sock.promise._res(p.clipValue);
                            }
                            break;
                    }
                },
                function (sock, err)
                {
                    try { sock.promise._rej('Protocol Error: ' + err); } catch (e) { }
                    try { sock.promise.server.close(); } catch (e) { }
                });
            });
            c.write(translateObject({ command: 'readClip' }));
        });

        return (ret);
    };
    this.lock = function lock()
    {
        // Start Local Server
        var ret = this._initIPCBase();
        ret.metadata = 'desktop/lock'
        ret.server = this.startMessageServer(ret);
        ret.server.ret = ret;
        ret.server.on('connection', function (c)
        {
            this._connection = c;
            c.promise = this.ret;
            c.on('end', function ()
            {
                // Peer closed without delivering a complete frame (e.g. helper killed mid-write).
                // Settle the promise instead of leaving it dangling; duplicate settles are
                // ignored by the promise implementation, so the normal flow is unaffected.
                try { this.promise._rej('Connection closed'); } catch (e) { }
                try { this.promise.server.close(); } catch (e) { }
            });
            c.on('data', function (buffer)
            {
                decodeFrames(this, buffer, function (sock, p)
                {
                    switch (p.command)
                    {
                        case 'ERROR':
                            sock.promise._rej(p.reason);
                            break;
                        case 'LOCK':
                            sock.promise._res();
                            break;
                    }
                },
                function (sock, err)
                {
                    try { sock.promise._rej('Protocol Error: ' + err); } catch (e) { }
                    try { sock.promise.server.close(); } catch (e) { }
                });
            });
            c.write(translateObject({ command: 'LOCK' }));
        });

        return (ret);
    };
    this.notify = function notify(title, caption)
    {
        if (title == 'MeshCentral') { try { title = require('MeshAgent').displayName; } catch (x) { } }
        if (require('user-sessions').isRoot())
        {
            var str = 'tell current application to display notification "' + caption + '" with title "' + title + '"';
            str = Buffer.from("console.log('" + str + "');process.exit();").toString('base64');

            var ret = new promise(function (res, rej) { this._res = res; this._rej = rej; });
            ret.uid = require('user-sessions').consoleUid();
            ret.name = require('user-sessions').getUsername(ret.uid);
            ret.child = require('child_process').execFile('/bin/zsh', ['zsh'], { type: require('child_process').SpawnTypes.TERM });
            ret.child.descriptorMetadata = "notify/toast";
            ret.child.promise = ret;
            ret.child.stderr.on('data', function () { });
            ret.child.stdout.on('data', function (c) { });
            ret.child.on('exit', function (code) { this.promise.child = null; this.promise._res('DISMISSED'); });

            ret.child.stdin.write('su - ' + ret.name + '\n');
            ret.child.stdin.write(process.execPath.split('./').join('').split(' ').join('\\ ') + ' -b64exec ' + str + ' | osascript\nexit\nexit\n');
            return (ret);
        }

        // Start Local Server
        var ret = this._initIPCBase();
        ret.metadata = 'notify'
        ret.title = title; ret.caption = caption; 
        ret.server = this.startMessageServer(ret);
        ret.server.ret = ret;
        ret.server.on('connection', function (c)
        {
            this._connection = c;
            c.promise = this.ret;
            c.on('end', function ()
            {
                // Peer closed without delivering a complete frame (e.g. helper killed mid-write).
                // Settle the promise instead of leaving it dangling; duplicate settles are
                // ignored by the promise implementation, so the normal flow is unaffected.
                try { this.promise._rej('Connection closed'); } catch (e) { }
                try { this.promise.server.close(); } catch (e) { }
            });
            c.on('data', function (buffer)
            {
                decodeFrames(this, buffer, function (sock, p)
                {
                    switch (p.command)
                    {
                        case 'ERROR':
                            sock.promise._rej(p.reason);
                            break;
                        case 'NOTIFY':

                            sock.promise._res();
                            break;
                    }
                },
                function (sock, err)
                {
                    try { sock.promise._rej('Protocol Error: ' + err); } catch (e) { }
                    try { sock.promise.server.close(); } catch (e) { }
                });
            });
            c.write(translateObject({ command: 'NOTIFY', title: this.ret.title, caption: this.ret.caption }));
        });

        return (ret);
    };
    this.startClient = function startClient(options)
    {
        // Create the Client
        options.osversion = require('service-manager').getOSVersion();
        options.uid = require('user-sessions').consoleUid();
        this.client = require('net').createConnection(options);
        this.client._options = options;
        // [2026-09-20 leanchiliu] HELLO 握手 + 重试：连上即发 HELLO，此后每 1s 重发，直到
        // 收到 DIALOG（收到即去重+停表）——首帧在 C 层投递丢失/悬挂的竞态靠重试兜底
        //（服务端每个 HELLO 都回一帧 DIALOG，DIALOG 端去重防多弹窗）
        this.client.on('connect', function ()
        {
            var sock = this;
            try { sock.write(translateObject({ command: 'HELLO' })); } catch (e) { }
            sock._helloTimer = setInterval(function () { try { sock.write(translateObject({ command: 'HELLO' })); } catch (e) { } }, 1000);
        });
        this.client.on('data', function (buffer)
        {
            decodeFrames(this, buffer, function (sock, p)
            {
                switch (p.command)
                {
                    case 'writeClip':
                        sock._shell = require('child_process').execFile('/usr/bin/pbcopy', ['pbcopy']);
                        sock._shell.ipc = sock;
                        sock._shell.stdout.str = ''; sock._shell.stdout.on('data', function (chunk) { this.str += chunk.toString(); });
                        sock._shell.stderr.str = ''; sock._shell.stderr.on('data', function (chunk) { this.str += chunk.toString(); });
                        sock._shell.stdin.write(p.clipText, function () { this.end(); });
                        sock._shell.on('exit', function ()
                        {
                            if (this.stderr.str != '')
                            {
                                this.ipc.end(translateObject({ command: 'writeClip', clipError: this.stderr.str }));
                            }
                            else
                            {
                                this.ipc.end(translateObject({ command: 'writeClip' }));
                            }
                        });
                        break;
                    case 'readClip':
                        sock._shell = require('child_process').execFile('/usr/bin/pbpaste', ['pbpaste']);
                        sock._shell.stdout.str = ''; sock._shell.stdout.on('data', function (chunk) { this.str += chunk.toString(); });
                        sock._shell.stderr.str = ''; sock._shell.stderr.on('data', function (chunk) { this.str += chunk.toString(); });
                        sock._shell.waitExit();
                        if (sock._shell.stderr.str != '')
                        {
                            sock.end(translateObject({ command: 'readClip', clipError: sock._shell.stderr.str }));
                        }
                        else
                        {
                            // if (sock._shell.stdout.str.length > 2) { sock._shell.stdout.str = sock._shell.stdout.str.substring(0, sock._shell.stdout.str.length - 2); }
                            sock.end(translateObject({ command: 'readClip', clipValue: sock._shell.stdout.str }));
                        }
                        break;
                    case 'LOCK':
                        sock._shell = require('child_process').execFile('/bin/sh', ['sh']);
                        sock._shell.stdout.str = ''; sock._shell.stdout.on('data', function (chunk) { this.str += chunk.toString(); });
                        sock._shell.stderr.str = ''; sock._shell.stderr.on('data', function (chunk) { this.str += chunk.toString(); });
                        sock._shell.stdin.write('/System/Library/CoreServices/Menu\\ Extras/User.menu/Contents/Resources/CGSession -suspend\nexit\n');
                        sock._shell.waitExit();
                        if (sock._shell.stderr.str != '')
                        {
                            sock.end(translateObject({ command: 'ERROR', reason: sock._shell.stderr.str }));
                        }
                        else
                        {
                            sock.end(translateObject({ command: 'LOCK', status: 0 }));
                        }
                        break;
                    case 'NOTIFY':
                        sock._shell = require('child_process').execFile('/bin/sh', ['sh']);
                        sock._shell.stdout.str = ''; sock._shell.stdout.on('data', function (chunk) { this.str += chunk.toString(); });
                        sock._shell.stderr.str = ''; sock._shell.stderr.on('data', function (chunk) { this.str += chunk.toString(); });
                        sock._shell.stdin.write('osascript -e \'tell current application to display notification "' + p.caption + '" with title "' + p.title + '"\'\nexit\n');
                        sock._shell.waitExit();
                        if (sock._shell.stderr.str != '')
                        {
                            sock.end(translateObject({ command: 'ERROR', reason: sock._shell.stderr.str }));
                        }
                        else
                        {
                            sock.end(translateObject({ command: 'NOTIFY', status: 0 }));
                        }
                        break;
                    case 'DIALOG':
                        // [2026-09-20] HELLO 重试会带来重复 DIALOG——去重 + 停 HELLO 定时器
                        if (sock._dialogActive) { break; }
                        sock._dialogActive = true;
                        if (sock._helloTimer) { try { clearInterval(sock._helloTimer); } catch (e) { } sock._helloTimer = null; }
                        var timeout = p.timeout ? (' giving up after ' + p.timeout) : '';
                        var icon = p.icon ? ('with icon ' + p.icon) : '';

                        var buttons = p.buttons ? ('buttons {' + p.buttons.toString() + '}') : '';
                        if (p.buttonDefault != null)
                        {
                            buttons += (' default button ' + p.buttonDefault)
                        }
                        sock._shell = require('child_process').execFile('/bin/sh', ['sh']);
                        sock._shell.that = sock;
                        sock._shell.stdout.str = ''; sock._shell.stdout.on('data', function (chunk) { this.str += chunk.toString(); });
                        sock._shell.stderr.str = ''; sock._shell.stderr.on('data', function (chunk) { this.str += chunk.toString(); });
                        sock._shell.stdin.write('osascript -e \'tell current application to display dialog "' + p.caption + '" with title "' + p.title + '" ' + icon + ' ' + buttons + timeout + '\' | awk \'{ c=split($0, tokens, ","); split(tokens[1], val, ":"); if(c==1) { print val[2] } else { split(tokens[2], gu, ":"); if(gu[2]=="true") { print "_TIMEOUT_" } else { print val[2]  }  } }\'\nexit\n');
                        sock._shell.on('exit', function ()
                        {
                            if (this.stderr.str != '' && !this.stderr.str.includes('OpenGL'))
                            {
                                if (this.stderr.str.includes('(-128)'))
                                {
                                    this.that.end(translateObject({ command: 'DIALOG', button: 'Cancel' }));
                                }
                                else
                                {
                                    this.that.end(translateObject({ command: 'ERROR', reason: this.stderr.str }));
                                }
                            }
                            else
                            {
                                if (this.stdout.str.trim() == '_TIMEOUT_')
                                {
                                    this.that.end(translateObject({ command: 'DIALOG', timeout: true }));
                                }
                                else
                                {
                                    this.that.end(translateObject({ command: 'DIALOG', button: this.stdout.str.trim() }));
                                }
                            }
                            this.that._shell = null;
                        });
                        sock.on('close', function ()
                        {
                            if (sock._shell) { sock._shell.kill(); }
                        });

                        //sock._shell.waitExit();
                        //if (sock._shell.stderr.str != '' && !sock._shell.stderr.str.includes('OpenGL'))
                        //{
                        //    sock.end(translateObject({ command: 'ERROR', reason: sock._shell.stderr.str }));
                        //}
                        //else
                        //{
                        //    if (sock._shell.stdout.str.trim() == '_TIMEOUT_')
                        //    {
                        //        sock.end(translateObject({ command: 'DIALOG', timeout: true }));
                        //    }
                        //    else
                        //    {
                        //        sock.end(translateObject({ command: 'DIALOG', button: sock._shell.stdout.str.trim() }));
                        //    }
                        //}
                        break;
                    default:
                        break;
                }
            },
            function (sock, err)
            {
                // [2026-09-20 leanchiliu] 坏帧容忍（同服务端口径）：不掐连接，等 HELLO
                // 重试送来好帧；外层 IPC 窗口超时兜底。
                try { sendConsoleText('message-box helper: 容忍坏帧(' + err + ')，等待重试'); } catch (e) { }
            });
        });
        this.client.on('error', function () { this.uninstall(); }).on('end', function () { this.uninstall(); });
        this.client.uninstall = function ()
        {
            if (this._uninstalled) { return; }
            this._uninstalled = true;
            // Need to uninstall ourselves.
            // NOTE (2026-09-18): Do NOT waitExit() here. This runs from the socket 'error'/'end'
            // handlers, which fire from within chain disconnect processing (ILibAsyncSocket_Disconnect
            // -> OnConnect -> emit). Blocking here re-enters the chain (nested ILibChain_Continue),
            // which re-dispatches the dying socket and recurses until the Duktape value stack is
            // exhausted -> SIGSEGV (deterministic crash whenever the unix socket path is dead when
            // the helper connects, e.g. launchd spawns the LaunchAgent after the parent's 5s
            // 'Connection timeout' already closed the server). The DETACHED child completes the
            // LaunchAgent removal on its own; the caller's 'end'/'error' wrapper exits the process.
            var child = require('child_process').execFile(process.execPath, [process.execPath.split('/').pop(), '-exec', "try { var s=require('service-manager').manager.getLaunchAgent('" + this._options.service + "', " + this._options.uid + "); s.unload(); require('fs').unlinkSync(s.plist); } catch (e) { } process.exit();"], { detached: true, type: require('child_process').SpawnTypes.DETACHED });
        };
        return (this.client);
    };
    this.startMessageServer = function startMessageServer(options)
    {
        if (require('fs').existsSync(options.path)) { require('fs').unlinkSync(options.path); }
        options.writableAll = true;

        var ret = require('net').createServer();
        ret.descriptorMetadata = ('[' + options.path + ']' + (options.metadata ? (', ' + options.metadata) : ''));
        ret.uid = require('user-sessions').consoleUid();
        ret.osversion = require('service-manager').getOSVersion();
        ret._options = options;
        // [2026-09-20 leanchiliu] IPC 连接窗口从固定 5s 改为跟随弹窗超时（+5s 余量）：
        // macOS 26 launchd 拉起 LaunchAgent 偶发 dyld 启动停滞（spec 实测 pre-main 卡
        // open 系统调用可达 100s+），5s 固定窗口在真机上常态性误杀慢启动的 helper
        //（'Connection timeout' reject）。窗口上界=弹窗 giving-up 秒数——弹窗都放弃了
        // IPC 再留着无意义。回退：恢复 5000。
        ret.timer = setTimeout(function (obj)
        {
            obj.close();
            obj._options._rej('Connection timeout');
        }, ((options.timeout && parseInt(options.timeout) > 0) ? (parseInt(options.timeout) * 1000 + 5000) : 35000), ret);
        ret.listen(options);
        ret.on('connection', function (c)
        {
            clearTimeout(this.timer);
            // [2026-09-20 leanchiliu] 回复截止兜底：连接建立即清外层窗口定时器，若 helper
            // 的回复帧丢失（真机实证 helper 连上、osascript 完成、回复不到达），此前无人
            // 兜底 → 无限悬挂（前端卡"等待同意"）。补一个与窗口同长的回复截止：期内无
            // DIALOG/ERROR 应答则按 TIMEOUT 结算。结算路径（onFrame 各 case / c.on('end')）
            // 负责 clearTimeout。
            var sv = this;
            if (sv._replyTimer != null) { clearTimeout(sv._replyTimer); }
            sv._replyTimer = setTimeout(function (obj)
            {
                try { obj._options._rej('TIMEOUT'); } catch (e) { }
                try { obj.close(); } catch (e) { }
            }, ((sv._options.timeout && parseInt(sv._options.timeout) > 0) ? (parseInt(sv._options.timeout) * 1000 + 5000) : 35000), sv);
        });
        ret.on('~', function ()
        {
            // [2026-09-20 leanchiliu] 裸 unlinkSync 在 finalizer 里抛 UncaughtException
            // （真机实录：close() 已删过 / 文件不存在时 ENOENT——"Error in Finalizer
            // (net.ipcServer)"）。finalizer 异常无消费方，吞掉即可。
            try { require('fs').unlinkSync(this._options.path); } catch (e) { }
        });

        // [2026-09-20 leanchiliu] helper 拉起双路径：非 root 且本进程已在控制台会话内 →
        // 直接 spawn 自身副本为 helper 子进程（继承本会话 GUI 上下文，osascript 可用，
        // 启动 ~40ms）——彻底绕开 launchd 拉起 LaunchAgent 的整类问题（macOS 26 dyld
        // pre-main 停滞实测 43s+/100s+ 且对无正式签名二进制每次启动评估，真机轮
        // 'Connection timeout' 反复复现）。root 生产形态（LaunchDaemon）不在控制台
        // 会话内，仍走 LaunchAgent 跨会话注入（配上方 IPC 窗口延长 + 正式签名交付）。
        // 回退：删分支恢复无条件 installLaunchAgent。
        // 自身会话判定用 Self()（mac 上 getProcessOwnerName 不存在，首版分支条件恒假走了 LaunchAgent 回退）
        var __self = null;
        try { __self = require('user-sessions').Self(); } catch (e) { }
        if (__self != null && __self == ret.uid)
        {
            try
            {
                var __exe = process.execPath; // agent fs 无 realpathSync，execPath 本为绝对路径
                // 注意两点：① agent execFile 的 args[0] 是 argv[0]（程序名），须占位——漏传时
                // 子进程把首参当程序名、其余当普通参数，-exec/-b64exec 均不生效（实测踩坑）；
                // ② 脚本用 -b64exec 传（b64 免引号转义，对齐模块内既有 zsh/osascript 拼接惯例）
                // [2026-09-20 临时诊断] helper 生命周期落文件（boot/connected/data/end/error）——真实流程定位连接/收帧失败；排查完还原
                var __b64 = Buffer.from("require('message-box').startClient({ path: '" + options.path + "', service: '" + options.tmpServiceName + "' }).on('end', function () { process.exit(); }).on('error', function () { process.exit(); });").toString('base64');
                var __child = require('child_process').execFile(__exe, [__exe.split('/').pop(), '-b64exec', __b64]);
                __child.descriptorMetadata = 'message-box/direct-helper';
            } catch (e) { }
        }
        else
        {
            require('service-manager').manager.installLaunchAgent(
                {
                    name: options.tmpServiceName, servicePath: process.execPath, startType: 'AUTO_START', uid: ret.uid,
                    sessionTypes: ['Aqua'], parameters: ['-exec', "require('message-box').startClient({ path: '" + options.path + "', service: '" + options.tmpServiceName + "' }).on('end', function () { process.exit(); }).on('error', function () { process.exit(); });"]
                });
            require('service-manager').manager.getLaunchAgent(options.tmpServiceName, ret.uid).load();
        }

        return (ret);
    };
}


switch(process.platform)
{
    case 'win32':
        module.exports = new messageBox();
        break;
    case 'linux':
    case 'freebsd':
        module.exports = new linux_messageBox();
        break;
    case 'darwin':
        module.exports = new macos_messageBox();
        break;
}






