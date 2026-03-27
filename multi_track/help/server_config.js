inlets = 1;
outlets = 1;

var is_loading = true;
var mode = 0;
var dir = '';
var conda_env = '';
var host = '';
var serverport = 7000;
var clientport = 8000;
var script = 'server.py';
var cuda = 0;
var client_os  = 'windows'; // OS of the machine running Max: 'windows', 'mac', 'linux'
var server_os  = 'linux';   // OS of the server machine:     'linux', 'mac', 'windows'
var ssh_port   = 22;        // SSH port (default 22, only injected when != 22)

function loaded() {
    is_loading = false;
    bang();
}

function set_mode(m) {
    mode = parseInt(m);
    if (!is_loading) bang();
}

function set() {
    var key = arguments[0];
    var parts = [];
    for (var i = 1; i < arguments.length; i++) parts.push(arguments[i]);
    var val = parts.join(' ');
    if      (key === 'dir')        dir        = val;
    else if (key === 'conda_env')  conda_env  = val;
    else if (key === 'host')       host       = val;
    else if (key === 'serverport') serverport = val;
    else if (key === 'clientport') clientport = val;
    else if (key === 'script')     script     = val;
    else if (key === 'cuda')       cuda       = val;
    else if (key === 'client_os')  client_os  = val;
    else if (key === 'server_os')  server_os  = val;
    else if (key === 'ssh_port')   ssh_port   = parseInt(val);
    // legacy alias
    else if (key === 'os')         client_os  = val;
    if (!is_loading) bang();
}

function bang() {
    function empty(v) { return !v || v === '""' || v === "''" || v === 'null' || v === 'bang'; }
    if (empty(dir))       { error("server_config: 'dir' is empty\n");       return; }
    if (empty(conda_env)) { error("server_config: 'conda_env' is empty\n"); return; }
    if (empty(serverport)){ error("server_config: 'serverport' is empty\n");return; }
    if (empty(clientport)){ error("server_config: 'clientport' is empty\n");return; }
    if (mode === 1 && empty(host)) { error("server_config: 'host' is empty (required for remote mode)\n"); return; }

    var cuda_val = (parseInt(cuda) === -1) ? "''" : cuda;
    if (parseInt(cuda) === -1) post("server_config: CUDA device -1, running on CPU\n");

    var cmd;

    if (mode === 0) {
        // ── Local mode ── client_os determines the shell ──────────────────────
        post("server_config: 'host' is ignored in local mode\n");

        if (client_os === 'mac') {
            cmd = 'zsh -ic "cd ~/' + dir
                + ' && conda run --no-capture-output -n ' + conda_env + ' python'
                + ' ' + script
                + ' --serverport ' + serverport
                + ' --clientport ' + clientport
                + ' --client_ip 127.0.0.1"';

        } else if (client_os === 'linux') {
            cmd = 'bash -ic "cd ~/' + dir
                + ' && conda run --no-capture-output -n ' + conda_env + ' python'
                + ' ' + script
                + ' --serverport ' + serverport
                + ' --clientport ' + clientport
                + ' --client_ip 127.0.0.1"';

        } else { // windows
            cmd = 'cmd.exe /K "cd %USERPROFILE%\\' + dir
                + ' && set CUDA_VISIBLE_DEVICES=' + cuda_val
                + ' && conda run --no-capture-output -n ' + conda_env + ' python'
                + ' ' + script
                + ' --serverport ' + serverport
                + ' --clientport ' + clientport
                + ' --client_ip 127.0.0.1"';
        }

    } else {
        // ── Remote mode ── server_os determines the shell inside SSH ──────────
        var ssh_p = (ssh_port !== 22) ? '-p ' + ssh_port + ' ' : '';

        if (server_os === 'mac') {
            // Mac server: zsh login shell
            cmd = 'ssh -t ' + ssh_p + host
                + " \"zsh -ic 'cd " + dir
                + ' && export CUDA_VISIBLE_DEVICES=' + cuda_val
                + ' && conda run --no-capture-output -n ' + conda_env + ' python'
                + ' ' + script
                + ' --serverport ' + serverport
                + " --clientport " + clientport + "'\"";

        } else if (server_os === 'windows') {
            // Windows server: OpenSSH starts in user home dir, so cd dir works without full path.
            // No inner \" escaping — keeps the last " clean so the C++ injector can insert --client_ip correctly.
            cmd = 'ssh -t ' + ssh_p + host
                + ' "cmd /C cd ' + dir
                + ' && set CUDA_VISIBLE_DEVICES=' + cuda_val
                + ' && conda run --no-capture-output -n ' + conda_env + ' python'
                + ' ' + script
                + ' --serverport ' + serverport
                + ' --clientport ' + clientport + '"';

        } else {
            // Linux server (default): bash login shell
            cmd = 'ssh -t ' + ssh_p + host
                + " \"bash -ic 'cd " + dir
                + ' && export CUDA_VISIBLE_DEVICES=' + cuda_val
                + ' && conda run --no-capture-output -n ' + conda_env + ' python'
                + ' ' + script
                + ' --serverport ' + serverport
                + " --clientport " + clientport + "'\"";
        }
    }

    outlet(0, cmd);
}
