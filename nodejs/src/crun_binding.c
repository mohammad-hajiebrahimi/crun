#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <regex.h> 

#include "crun_binding.h"


static int binding_initialized = 0;

char* napi_get_string(napi_env env, napi_value value) {
    napi_valuetype type;
    napi_typeof(env, value, &type);
    
    if (type != napi_string) {
        return NULL;
    }
    
    size_t len;
    napi_get_value_string_utf8(env, value, NULL, 0, &len);
    
    char* str = (char*)malloc(len + 1);
    if (!str) return NULL;
    
    napi_get_value_string_utf8(env, value, str, len + 1, &len);
    return str;
}

static char* get_string_property(napi_env env, napi_value obj, const char* key) {
    napi_value val;
    bool has_prop;
    
    napi_has_named_property(env, obj, key, &has_prop);
    if (!has_prop) return NULL;
    
    napi_get_named_property(env, obj, key, &val);
    return napi_get_string(env, val);
}

static bool get_bool_property(napi_env env, napi_value obj, const char* key, bool default_val) {
    napi_value val;
    bool has_prop, result;
    
    napi_has_named_property(env, obj, key, &has_prop);
    if (!has_prop) return default_val;
    
    napi_get_named_property(env, obj, key, &val);
    napi_get_value_bool(env, val, &result);
    return result;
}

napi_value napi_create_error_obj(napi_env env, int code, const char* msg) {
    napi_value obj, val;
    
    napi_create_object(env, &obj);
    
    napi_create_int32(env, code, &val);
    napi_set_named_property(env, obj, "code", val);
    
    napi_create_string_utf8(env, msg ? msg : "Unknown error", NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, obj, "message", val);
    
    napi_get_boolean(env, true, &val);
    napi_set_named_property(env, obj, "error", val);
    
    return obj;
}

static napi_value create_error_from_crun(napi_env env, const char* prefix, libcrun_error_t* err) {
    char msg[2048];
    
    if (err && *err) {
        snprintf(msg, sizeof(msg), "%s: %s", prefix, (*err)->msg);
        libcrun_error_release(err);
    } else {
        snprintf(msg, sizeof(msg), "%s: Unknown error", prefix);
    }
    
    return napi_create_error_obj(env, -1, msg);
}

libcrun_context_t* create_context(const char* id, const char* state_root) {
    libcrun_context_t* ctx = (libcrun_context_t*)calloc(1, sizeof(libcrun_context_t));
    if (!ctx) return NULL;
    
    ctx->id = id ? strdup(id) : NULL;
    ctx->state_root = state_root ? strdup(state_root) : strdup("/run/crun");
    ctx->systemd_cgroup = 0;
    ctx->force_no_cgroup = 0;
    ctx->notify_socket = NULL;
    ctx->handler = NULL;
    ctx->fifo_exec_wait_fd = -1;
    ctx->preserve_fds = 0;
    ctx->detach = 1;
    ctx->no_new_keyring = 0;
    
    return ctx;
}

void free_context(libcrun_context_t* ctx) {
    if (!ctx) return;
    
    if (ctx->id) free((void*)ctx->id);
    if (ctx->state_root) free((void*)ctx->state_root);
    if (ctx->bundle) free((void*)ctx->bundle);
    if (ctx->console_socket) free((void*)ctx->console_socket);
    if (ctx->pid_file) free((void*)ctx->pid_file);
    if (ctx->notify_socket) free((void*)ctx->notify_socket);
    
    free(ctx);
}

static int signal_from_name(const char* name) {
    if (!name) return SIGTERM;
    
    if (strcasecmp(name, "SIGTERM") == 0 || strcasecmp(name, "TERM") == 0) return SIGTERM;
    if (strcasecmp(name, "SIGKILL") == 0 || strcasecmp(name, "KILL") == 0) return SIGKILL;
    if (strcasecmp(name, "SIGINT") == 0 || strcasecmp(name, "INT") == 0) return SIGINT;
    if (strcasecmp(name, "SIGHUP") == 0 || strcasecmp(name, "HUP") == 0) return SIGHUP;
    if (strcasecmp(name, "SIGSTOP") == 0 || strcasecmp(name, "STOP") == 0) return SIGSTOP;
    if (strcasecmp(name, "SIGCONT") == 0 || strcasecmp(name, "CONT") == 0) return SIGCONT;
    if (strcasecmp(name, "SIGUSR1") == 0 || strcasecmp(name, "USR1") == 0) return SIGUSR1;
    if (strcasecmp(name, "SIGUSR2") == 0 || strcasecmp(name, "USR2") == 0) return SIGUSR2;
    
    return SIGTERM;
}


/**
 * create(id, bundle, options?) -> {id, status, bundle, pid, error}
 *
 * @param {string} id - شناسه کانتینر
 * @param {string} bundle - مسیر bundle
 * @param {object} [options] - تنظیمات اختیاری
 * @param {string} [options.stateRoot] - مسیر state root
 * @param {boolean} [options.systemdCgroup] - استفاده از systemd cgroup
 * @param {string} [options.consoleSocket] - مسیر socket برای tty
 * @param {string} [options.pidFile] - مسیر فایل PID
 * @param {string} [options.configFile] - نام فایل config (پیش‌فرض: config.json)
 * @param {number} [options.preserveFds] - تعداد FD های اضافی
 * @param {boolean} [options.noPivot] - عدم استفاده از pivot_root
 * @param {boolean} [options.noNewKeyring] - حفظ session key
 */
napi_value CrunCreate(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3], result;

    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, NULL, NULL));

    if (argc < 2) {
        return napi_create_error_obj(env, -1, "please specify a ID and bundle path");
    }

    char* id = napi_get_string(env, args[0]);
    char* bundle_arg = napi_get_string(env, args[1]);

    if (!id || !bundle_arg) {
        free(id);
        free(bundle_arg);
        return napi_create_error_obj(env, -1, "Invalid id or bundle path");
    }

    char* state_root = NULL;
    char* console_socket = NULL;
    char* pid_file = NULL;
    char* config_file_arg = NULL;
    bool systemd_cgroup = false;
    bool no_pivot = false;
    bool no_new_keyring = false;
    int preserve_fds = 0;

    if (argc >= 3) {
        napi_valuetype type;
        napi_typeof(env, args[2], &type);

        if (type == napi_object) {
            state_root = get_string_property(env, args[2], "stateRoot");
            console_socket = get_string_property(env, args[2], "consoleSocket");
            pid_file = get_string_property(env, args[2], "pidFile");
            config_file_arg = get_string_property(env, args[2], "configFile");
            systemd_cgroup = get_bool_property(env, args[2], "systemdCgroup", false);
            no_pivot = get_bool_property(env, args[2], "noPivot", false);
            no_new_keyring = get_bool_property(env, args[2], "noNewKeyring", false);

            napi_value pfd_val;
            bool has_pfd;
            napi_has_named_property(env, args[2], "preserveFds", &has_pfd);
            if (has_pfd) {
                napi_get_named_property(env, args[2], "preserveFds", &pfd_val);
                napi_get_value_int32(env, pfd_val, &preserve_fds);
            }
        }
    }

    char* original_cwd = getcwd(NULL, 0);


    const char* config_file = "config.json";
    char* config_file_cleanup = NULL;

    if (config_file_arg && strcmp("config.json", config_file_arg) != 0) {
        if (config_file_arg[0] != '/') {
            config_file_cleanup = realpath(config_file_arg, NULL);
            if (!config_file_cleanup) {
                if (original_cwd) free(original_cwd);
                free(id); free(bundle_arg); free(state_root);
                free(console_socket); free(pid_file); free(config_file_arg);
                char msg[256];
                snprintf(msg, sizeof(msg), "realpath `%s` failed", config_file_arg);
                return napi_create_error_obj(env, errno, msg);
            }
            config_file = config_file_cleanup;
        } else {
            config_file = config_file_arg;
        }
    }

    const char* bundle = NULL;
    char* bundle_cleanup = NULL;

    if (bundle_arg[0] != '/') {
        bundle_cleanup = realpath(bundle_arg, NULL);
        if (!bundle_cleanup) {
            if (original_cwd) free(original_cwd);
            free(id); free(bundle_arg); free(state_root);
            free(console_socket); free(pid_file);
            free(config_file_arg); free(config_file_cleanup);
            char msg[256];
            snprintf(msg, sizeof(msg), "realpath `%s` failed", bundle_arg);
            return napi_create_error_obj(env, errno, msg);
        }
        bundle = bundle_cleanup;
    } else {
        bundle = bundle_arg;
    }

    if (chdir(bundle) < 0) {
        int e = errno;
        if (original_cwd) free(original_cwd);
        free(id); free(bundle_arg); free(bundle_cleanup);
        free(state_root); free(console_socket);
        free(pid_file); free(config_file_arg); free(config_file_cleanup);
        char msg[256];
        snprintf(msg, sizeof(msg), "chdir `%s` failed", bundle);
        return napi_create_error_obj(env, e, msg);
    }

    libcrun_context_t crun_context = {0};

    crun_context.id = id;
    crun_context.state_root = state_root;
    crun_context.systemd_cgroup = systemd_cgroup ? 1 : 0;
    crun_context.no_pivot = no_pivot;
    crun_context.no_new_keyring = no_new_keyring;
    crun_context.preserve_fds = preserve_fds;
    crun_context.listen_fds = 0;
    crun_context.fifo_exec_wait_fd = -1;
    crun_context.detach = 1;

    if (console_socket)
        crun_context.console_socket = console_socket;
    if (pid_file)
        crun_context.pid_file = pid_file;


    libcrun_error_t err = NULL;
    libcrun_container_t* container = libcrun_container_load_from_file(config_file, &err);

    if (!container) {
        napi_value error = create_error_from_crun(env, "error loading config.json", &err);
        if (original_cwd) {
            int r __attribute__((unused)) = chdir(original_cwd);
            free(original_cwd);
        }
        free(id); free(bundle_arg); free(bundle_cleanup);
        free(state_root); free(console_socket);
        free(pid_file); free(config_file_arg); free(config_file_cleanup);
        return error;
    }


    crun_context.bundle = bundle;

    if (getenv("LISTEN_FDS")) {
        crun_context.listen_fds = strtoll(getenv("LISTEN_FDS"), NULL, 10);
        crun_context.preserve_fds += crun_context.listen_fds;
    }
    int ret = libcrun_container_create(&crun_context, container, 0, &err);

    libcrun_container_free(container);

    if (original_cwd) {
        int r __attribute__((unused)) = chdir(original_cwd);
        free(original_cwd);
    }

    if (ret < 0) {
        napi_value error = create_error_from_crun(env, "Failed to create container", &err);
        free(id); free(bundle_arg); free(bundle_cleanup);
        free(state_root); free(console_socket);
        free(pid_file); free(config_file_arg); free(config_file_cleanup);
        return error;
    }

    napi_create_object(env, &result);

    napi_value val;

    napi_create_string_utf8(env, id, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, result, "id", val);

    napi_create_string_utf8(env, "created", NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, result, "status", val);

    napi_create_string_utf8(env, bundle, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, result, "bundle", val);

    napi_create_int32(env, ret, &val);
    napi_set_named_property(env, result, "pid", val);

    napi_get_boolean(env, false, &val);
    napi_set_named_property(env, result, "error", val);

    /* آزادسازی */
    free(id); free(bundle_arg); free(bundle_cleanup);
    free(state_root); free(console_socket);
    free(pid_file); free(config_file_arg); free(config_file_cleanup);

    return result;
}
/**
 * start(id, options?) -> {id, status, pid, error}
 * 
 * @param {string} id - شناسه کانتینر
 * @param {object} [options] - تنظیمات اختیاری
 * @param {string} [options.stateRoot] - مسیر state root
 * @param {boolean} [options.systemdCgroup] - استفاده از systemd cgroup
 */
napi_value CrunStart(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2], result;
    
    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, NULL, NULL));
    
    if (argc < 1) {
        return napi_create_error_obj(env, -1, "please specify a ID for the container");
    }
    
    char* id = napi_get_string(env, args[0]);
    if (!id) {
        return napi_create_error_obj(env, -1, "Invalid container id");
    }

    char* state_root = NULL;
    bool systemd_cgroup = false;
    
    if (argc >= 2) {
        napi_valuetype type;
        napi_typeof(env, args[1], &type);
        if (type == napi_object) {
            state_root = get_string_property(env, args[1], "stateRoot");
            systemd_cgroup = get_bool_property(env, args[1], "systemdCgroup", false);
        }
    }
    
    libcrun_context_t crun_context = {0};
    
    crun_context.id = id;
    crun_context.state_root = state_root;
    crun_context.systemd_cgroup = systemd_cgroup ? 1 : 0;
    crun_context.fifo_exec_wait_fd = -1;

    libcrun_error_t err = NULL;
    int ret = libcrun_container_start(&crun_context, id, &err);
    
    if (ret < 0) {
        napi_value error = create_error_from_crun(env, "Failed to start container", &err);
        free(id);
        free(state_root);
        return error;
    }
    
    napi_create_object(env, &result);
    
    napi_value val;
    
    napi_create_string_utf8(env, id, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, result, "id", val);
    
    napi_create_string_utf8(env, "running", NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, result, "status", val);
    
    napi_create_int32(env, ret, &val);
    napi_set_named_property(env, result, "pid", val);
    
    napi_get_boolean(env, false, &val);
    napi_set_named_property(env, result, "error", val);
    
    /* آزادسازی حافظه */
    free(id);
    free(state_root);
    
    return result;
}

/**
 * run(id, bundle, options?) -> {id, status, bundle, pid, error}
 * 
 * @param {string} id - شناسه کانتینر
 * @param {string} bundle - مسیر bundle
 * @param {object} [options] - تنظیمات اختیاری
 * @param {string} [options.stateRoot] - مسیر state root
 * @param {boolean} [options.systemdCgroup] - استفاده از systemd cgroup
 * @param {string} [options.consoleSocket] - مسیر socket برای tty
 * @param {string} [options.pidFile] - مسیر فایل PID
 * @param {string} [options.configFile] - نام فایل config (پیش‌فرض: config.json)
 * @param {number} [options.preserveFds] - تعداد FD های اضافی
 * @param {boolean} [options.detach] - اجرا در پس‌زمینه (پیش‌فرض: true)
 * @param {boolean} [options.noPivot] - عدم استفاده از pivot_root
 * @param {boolean} [options.noNewKeyring] - حفظ session key
 */
napi_value CrunRun(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3], result;
    
    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, NULL, NULL));
    
    if (argc < 2) {
        return napi_create_error_obj(env, -1, "please specify a ID and bundle path");
    }
    
    char* id = napi_get_string(env, args[0]);
    char* bundle_arg = napi_get_string(env, args[1]);
    
    if (!id || !bundle_arg) {
        free(id);
        free(bundle_arg);
        return napi_create_error_obj(env, -1, "Invalid id or bundle path");
    }
    
    char* state_root = NULL;
    char* console_socket = NULL;
    char* pid_file = NULL;
    char* config_file_arg = NULL;
    bool systemd_cgroup = false;
    bool detach = true;
    bool no_pivot = false;
    bool no_new_keyring = false;
    int preserve_fds = 0;
    
    if (argc >= 3) {
        napi_valuetype type;
        napi_typeof(env, args[2], &type);
        
        if (type == napi_object) {
            state_root = get_string_property(env, args[2], "stateRoot");
            console_socket = get_string_property(env, args[2], "consoleSocket");
            pid_file = get_string_property(env, args[2], "pidFile");
            config_file_arg = get_string_property(env, args[2], "configFile");
            systemd_cgroup = get_bool_property(env, args[2], "systemdCgroup", false);
            detach = get_bool_property(env, args[2], "detach", true);
            no_pivot = get_bool_property(env, args[2], "noPivot", false);
            no_new_keyring = get_bool_property(env, args[2], "noNewKeyring", false);
            
            napi_value pfd_val;
            bool has_pfd;
            napi_has_named_property(env, args[2], "preserveFds", &has_pfd);
            if (has_pfd) {
                napi_get_named_property(env, args[2], "preserveFds", &pfd_val);
                napi_get_value_int32(env, pfd_val, &preserve_fds);
            }
        }
    }

    char* original_cwd = getcwd(NULL, 0);
    
    const char* config_file = "config.json";
    char* config_file_cleanup = NULL;
    
    if (config_file_arg && strcmp("config.json", config_file_arg) != 0) {
        if (config_file_arg[0] != '/') {
            config_file_cleanup = realpath(config_file_arg, NULL);
            if (!config_file_cleanup) {
                if (original_cwd) free(original_cwd);
                free(id); free(bundle_arg); free(state_root);
                free(console_socket); free(pid_file); free(config_file_arg);
                return napi_create_error_obj(env, errno, "realpath config failed");
            }
            config_file = config_file_cleanup;
        } else {
            config_file = config_file_arg;
        }
    }
    
    const char* bundle = NULL;
    char* bundle_cleanup = NULL;
    
    if (bundle_arg[0] != '/') {
        bundle_cleanup = realpath(bundle_arg, NULL);
        if (!bundle_cleanup) {
            if (original_cwd) free(original_cwd);
            free(id); free(bundle_arg); free(state_root);
            free(console_socket); free(pid_file);
            free(config_file_arg); free(config_file_cleanup);
            return napi_create_error_obj(env, errno, "realpath bundle failed");
        }
        bundle = bundle_cleanup;
    } else {
        bundle = bundle_arg;
    }
    
    if (chdir(bundle) < 0) {
        int e = errno;
        if (original_cwd) free(original_cwd);
        free(id); free(bundle_arg); free(bundle_cleanup);
        free(state_root); free(console_socket);
        free(pid_file); free(config_file_arg); free(config_file_cleanup);
        return napi_create_error_obj(env, e, "chdir to bundle failed");
    }
    
    libcrun_error_t err = NULL;
    
    libcrun_container_t* container = libcrun_container_load_from_file(config_file, &err);
    
    if (!container) {
        napi_value error = create_error_from_crun(env, "error loading config.json", &err);
        if (original_cwd) {
            int r __attribute__((unused)) = chdir(original_cwd);
            free(original_cwd);
        }
        free(id); free(bundle_arg); free(bundle_cleanup);
        free(state_root); free(console_socket);
        free(pid_file); free(config_file_arg); free(config_file_cleanup);
        return error;
    }
    
    libcrun_context_t crun_context = {0};
    
    crun_context.id = id;
    crun_context.bundle = bundle;
    crun_context.state_root = state_root;
    crun_context.systemd_cgroup = systemd_cgroup ? 1 : 0;
    crun_context.detach = detach ? 1 : 0;
    crun_context.no_pivot = no_pivot;
    crun_context.no_new_keyring = no_new_keyring;
    crun_context.preserve_fds = preserve_fds;
    crun_context.listen_fds = 0;
    crun_context.fifo_exec_wait_fd = -1;
    
    if (console_socket)
        crun_context.console_socket = console_socket;
    if (pid_file)
        crun_context.pid_file = pid_file;
    
    if (getenv("LISTEN_FDS")) {
        crun_context.listen_fds = strtoll(getenv("LISTEN_FDS"), NULL, 10);
        crun_context.preserve_fds += crun_context.listen_fds;
    }
    
    int ret = libcrun_container_run(&crun_context, container, 0, &err);
    
    libcrun_container_free(container);
    
    if (original_cwd) {
        int r __attribute__((unused)) = chdir(original_cwd);
        free(original_cwd);
    }
    
    if (ret < 0) {
        napi_value error = create_error_from_crun(env, "Failed to run container", &err);
        free(id); free(bundle_arg); free(bundle_cleanup);
        free(state_root); free(console_socket);
        free(pid_file); free(config_file_arg); free(config_file_cleanup);
        return error;
    }
    
    napi_create_object(env, &result);
    
    napi_value val;
    
    napi_create_string_utf8(env, id, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, result, "id", val);
    
    napi_create_string_utf8(env, "running", NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, result, "status", val);
    
    napi_create_string_utf8(env, bundle, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, result, "bundle", val);
    
    napi_create_int32(env, ret, &val);
    napi_set_named_property(env, result, "pid", val);
    
    napi_get_boolean(env, false, &val);
    napi_set_named_property(env, result, "error", val);
    
    free(id); free(bundle_arg); free(bundle_cleanup);
    free(state_root); free(console_socket);
    free(pid_file); free(config_file_arg); free(config_file_cleanup);
    
    return result;
}

/**
 * kill(id, signal?, options?) -> {id, signal, success, error}
 * 
 * @param {string} id - شناسه کانتینر (یا regex اگر options.regex=true)
 * @param {string|number} [signal='SIGTERM'] - سیگنال
 * @param {object} [options] - تنظیمات اختیاری
 * @param {string} [options.stateRoot] - مسیر state root
 * @param {boolean} [options.systemdCgroup] - استفاده از systemd cgroup
 * @param {boolean} [options.all] - ارسال سیگنال به همه پروسه‌ها
 * @param {boolean} [options.regex] - id به عنوان regex استفاده شود
 */
napi_value CrunKill(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3], result;
    
    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, NULL, NULL));
    
    if (argc < 1) {
        return napi_create_error_obj(env, -1, "please specify a ID for the container");
    }
    
    char* id = napi_get_string(env, args[0]);
    if (!id) {
        return napi_create_error_obj(env, -1, "Invalid container id");
    }
    
    char sig_str_buf[32] = "SIGTERM";
    
    if (argc >= 2) {
        napi_valuetype type;
        napi_typeof(env, args[1], &type);
        
        if (type == napi_number) {
            int sig_num;
            napi_get_value_int32(env, args[1], &sig_num);
            snprintf(sig_str_buf, sizeof(sig_str_buf), "%d", sig_num);
        } else if (type == napi_string) {
            char* tmp = napi_get_string(env, args[1]);
            if (tmp) {
                strncpy(sig_str_buf, tmp, sizeof(sig_str_buf) - 1);
                sig_str_buf[sizeof(sig_str_buf) - 1] = '\0';
                free(tmp);
            }
        }
    }
    
    char* state_root = NULL;
    bool systemd_cgroup = false;
    bool kill_all = false;
    bool use_regex = false;
    
    if (argc >= 3) {
        napi_valuetype type;
        napi_typeof(env, args[2], &type);
        if (type == napi_object) {
            state_root = get_string_property(env, args[2], "stateRoot");
            systemd_cgroup = get_bool_property(env, args[2], "systemdCgroup", false);
            kill_all = get_bool_property(env, args[2], "all", false);
            use_regex = get_bool_property(env, args[2], "regex", false);
        }
    }
    
    libcrun_context_t crun_context = {0};
    
    crun_context.id = id;
    crun_context.state_root = state_root;
    crun_context.systemd_cgroup = systemd_cgroup ? 1 : 0;
    crun_context.fifo_exec_wait_fd = -1;
    
    libcrun_error_t err = NULL;
    int ret;
    int killed_count = 0;
    
    if (use_regex) {
        regex_t re;
        
        ret = regcomp(&re, id, REG_EXTENDED | REG_NOSUB);
        if (ret != 0) {
            free(id);
            free(state_root);
            return napi_create_error_obj(env, -1, "invalid regular expression");
        }
        
        const char* root = state_root ? state_root : "/run/crun";
        libcrun_container_list_t* list = NULL;
        
        ret = libcrun_get_containers_list(&list, root, &err);
        if (ret < 0) {
            regfree(&re);
            napi_value error = create_error_from_crun(env, "cannot read containers list", &err);
            free(id);
            free(state_root);
            return error;
        }
        
        libcrun_container_list_t* it;
        for (it = list; it; it = it->next) {
            if (regexec(&re, it->name, 0, NULL, 0) == 0) {
                libcrun_error_t kill_err = NULL;
                int kill_ret = libcrun_container_kill(&crun_context, it->name, sig_str_buf, &kill_err);
                if (kill_ret >= 0) {
                    killed_count++;
                } else if (kill_err) {
                    libcrun_error_release(&kill_err);
                }
            }
        }
        
        libcrun_free_containers_list(list);
        regfree(&re);

        napi_create_object(env, &result);
        
        napi_value val;
        napi_create_string_utf8(env, id, NAPI_AUTO_LENGTH, &val);
        napi_set_named_property(env, result, "pattern", val);
        
        napi_create_string_utf8(env, sig_str_buf, NAPI_AUTO_LENGTH, &val);
        napi_set_named_property(env, result, "signal", val);
        
        napi_create_int32(env, killed_count, &val);
        napi_set_named_property(env, result, "killedCount", val);
        
        napi_get_boolean(env, true, &val);
        napi_set_named_property(env, result, "success", val);
        
        napi_get_boolean(env, false, &val);
        napi_set_named_property(env, result, "error", val);
        
        free(id);
        free(state_root);
        return result;
    }
    
    if (kill_all) {
        ret = libcrun_container_killall(&crun_context, id, sig_str_buf, &err);
    } else {
        ret = libcrun_container_kill(&crun_context, id, sig_str_buf, &err);
    }
    
    if (ret < 0) {
        napi_value error = create_error_from_crun(env, "Failed to kill container", &err);
        free(id);
        free(state_root);
        return error;
    }
    
    napi_create_object(env, &result);
    
    napi_value val;
    
    napi_create_string_utf8(env, id, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, result, "id", val);
    
    napi_create_string_utf8(env, sig_str_buf, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, result, "signal", val);
    
    napi_get_boolean(env, kill_all, &val);
    napi_set_named_property(env, result, "all", val);
    
    napi_get_boolean(env, true, &val);
    napi_set_named_property(env, result, "success", val);
    
    napi_get_boolean(env, false, &val);
    napi_set_named_property(env, result, "error", val);
    
    free(id);
    free(state_root);
    
    return result;
}


/**
 * delete(id, options?) -> {id, deleted, error} | {pattern, deletedCount, error}
 * 
 * @param {string} id - شناسه کانتینر (یا regex اگر options.regex=true)
 * @param {object} [options] - تنظیمات اختیاری
 * @param {string} [options.stateRoot] - مسیر state root
 * @param {boolean} [options.systemdCgroup] - استفاده از systemd cgroup
 * @param {boolean} [options.force] - حذف حتی اگر در حال اجراست
 * @param {boolean} [options.regex] - id به عنوان regex استفاده شود
 */
napi_value CrunDelete(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2], result;
    
    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, NULL, NULL));
    
    if (argc < 1) {
        return napi_create_error_obj(env, -1, "please specify a ID for the container");
    }
    
    char* id = napi_get_string(env, args[0]);
    if (!id) {
        return napi_create_error_obj(env, -1, "Invalid container id");
    }

    char* state_root = NULL;
    bool systemd_cgroup = false;
    bool force = false;
    bool use_regex = false;
    
    if (argc >= 2) {
        napi_valuetype type;
        napi_typeof(env, args[1], &type);
        if (type == napi_object) {
            state_root = get_string_property(env, args[1], "stateRoot");
            systemd_cgroup = get_bool_property(env, args[1], "systemdCgroup", false);
            force = get_bool_property(env, args[1], "force", false);
            use_regex = get_bool_property(env, args[1], "regex", false);
        }
    }

    libcrun_context_t crun_context = {0};
    
    crun_context.id = id;
    crun_context.state_root = state_root;
    crun_context.systemd_cgroup = systemd_cgroup ? 1 : 0;
    crun_context.fifo_exec_wait_fd = -1;
    
    libcrun_error_t err = NULL;
    int ret;
    

    if (use_regex) {
        regex_t re;
        
        ret = regcomp(&re, id, REG_EXTENDED | REG_NOSUB);
        if (ret != 0) {
            free(id);
            free(state_root);
            return napi_create_error_obj(env, -1, "invalid regular expression");
        }
        
        const char* root = state_root ? state_root : "/run/crun";
        libcrun_container_list_t* list = NULL;
        
        ret = libcrun_get_containers_list(&list, root, &err);
        if (ret < 0) {
            regfree(&re);
            napi_value error = create_error_from_crun(env, "cannot read containers list", &err);
            free(id);
            free(state_root);
            return error;
        }

        int deleted_count = 0;
        libcrun_container_list_t* it;
        for (it = list; it; it = it->next) {
            if (regexec(&re, it->name, 0, NULL, 0) == 0) {
                libcrun_error_t del_err = NULL;
                int del_ret = libcrun_container_delete(&crun_context, NULL, it->name, force, &del_err);
                if (del_ret >= 0) {
                    deleted_count++;
                } else if (del_err) {
                    libcrun_error_release(&del_err);
                }
            }
        }
        
        libcrun_free_containers_list(list);
        regfree(&re);

        napi_create_object(env, &result);
        
        napi_value val;
        napi_create_string_utf8(env, id, NAPI_AUTO_LENGTH, &val);
        napi_set_named_property(env, result, "pattern", val);
        
        napi_create_int32(env, deleted_count, &val);
        napi_set_named_property(env, result, "deletedCount", val);
        
        napi_get_boolean(env, force, &val);
        napi_set_named_property(env, result, "force", val);
        
        napi_get_boolean(env, true, &val);
        napi_set_named_property(env, result, "deleted", val);
        
        napi_get_boolean(env, false, &val);
        napi_set_named_property(env, result, "error", val);
        
        free(id);
        free(state_root);
        return result;
    }

    ret = libcrun_container_delete(&crun_context, NULL, id, force, &err);
    
    if (ret < 0) {
        napi_value error = create_error_from_crun(env, "Failed to delete container", &err);
        free(id);
        free(state_root);
        return error;
    }

    napi_create_object(env, &result);
    
    napi_value val;
    
    napi_create_string_utf8(env, id, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, result, "id", val);
    
    napi_get_boolean(env, force, &val);
    napi_set_named_property(env, result, "force", val);
    
    napi_get_boolean(env, true, &val);
    napi_set_named_property(env, result, "deleted", val);
    
    napi_get_boolean(env, false, &val);
    napi_set_named_property(env, result, "error", val);

    free(id);
    free(state_root);
    
    return result;
}

/**
 * state(id, options?) -> {ociVersion, id, status, pid, bundle, ...}
 * 
 * @param {string} id - شناسه کانتینر
 * @param {object} [options] - تنظیمات اختیاری
 * @param {string} [options.stateRoot] - مسیر state root
 * @param {boolean} [options.systemdCgroup] - استفاده از systemd cgroup
 */
napi_value CrunState(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2], result;
    
    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, NULL, NULL));
    
    if (argc < 1) {
        return napi_create_error_obj(env, -1, "please specify a ID for the container");
    }
    
    char* id = napi_get_string(env, args[0]);
    if (!id) {
        return napi_create_error_obj(env, -1, "Invalid container id");
    }
    
    char* state_root = NULL;
    bool systemd_cgroup = false;
    
    if (argc >= 2) {
        napi_valuetype type;
        napi_typeof(env, args[1], &type);
        if (type == napi_object) {
            state_root = get_string_property(env, args[1], "stateRoot");
            systemd_cgroup = get_bool_property(env, args[1], "systemdCgroup", false);
        }
    }
    
    libcrun_context_t crun_context = {0};
    
    crun_context.id = id;
    crun_context.state_root = state_root;
    crun_context.systemd_cgroup = systemd_cgroup ? 1 : 0;
    crun_context.fifo_exec_wait_fd = -1;

    char* json_output = NULL;
    size_t json_size = 0;
    FILE* memstream = open_memstream(&json_output, &json_size);
    
    if (!memstream) {
        free(id);
        free(state_root);
        return napi_create_error_obj(env, -1, "Failed to create memory stream");
    }

    libcrun_error_t err = NULL;
    int ret = libcrun_container_state(&crun_context, id, memstream, &err);
    fclose(memstream);
    
    if (ret < 0) {
        napi_value error = create_error_from_crun(env, "Failed to get container state", &err);
        free(json_output);
        free(id);
        free(state_root);
        return error;
    }
    
    napi_create_object(env, &result);
    
    napi_value val;

    if (json_output && json_size > 0) {
        napi_create_string_utf8(env, json_output, json_size, &val);
    } else {
        napi_create_string_utf8(env, "{}", NAPI_AUTO_LENGTH, &val);
    }
    napi_set_named_property(env, result, "stateJson", val);
    
    libcrun_container_status_t status = {0};
    const char* root = state_root ? state_root : "/run/crun";
    
    int status_ret = libcrun_read_container_status(&status, root, id, NULL);
    
    if (status_ret >= 0) {
        int running = libcrun_is_container_running(&status, NULL);
        
        napi_create_string_utf8(env, "1.0.2", NAPI_AUTO_LENGTH, &val);
        napi_set_named_property(env, result, "ociVersion", val);
        
        napi_create_string_utf8(env, id, NAPI_AUTO_LENGTH, &val);
        napi_set_named_property(env, result, "id", val);
        
        const char* status_str;
        if (running > 0)
            status_str = "running";
        else if (status.pid > 0)
            status_str = "stopped";
        else
            status_str = "stopped";
        
        napi_create_string_utf8(env, status_str, NAPI_AUTO_LENGTH, &val);
        napi_set_named_property(env, result, "status", val);
        
        napi_create_int32(env, status.pid, &val);
        napi_set_named_property(env, result, "pid", val);
        
        if (status.bundle) {
            napi_create_string_utf8(env, status.bundle, NAPI_AUTO_LENGTH, &val);
            napi_set_named_property(env, result, "bundle", val);
        }
        
        if (status.rootfs) {
            napi_create_string_utf8(env, status.rootfs, NAPI_AUTO_LENGTH, &val);
            napi_set_named_property(env, result, "rootfs", val);
        }
        
        if (status.created) {
            napi_create_string_utf8(env, status.created, NAPI_AUTO_LENGTH, &val);
            napi_set_named_property(env, result, "created", val);
        }
        
        if (status.owner) {
            napi_create_string_utf8(env, status.owner, NAPI_AUTO_LENGTH, &val);
            napi_set_named_property(env, result, "owner", val);
        }
        
        libcrun_free_container_status(&status);
    } else {
        napi_create_string_utf8(env, id, NAPI_AUTO_LENGTH, &val);
        napi_set_named_property(env, result, "id", val);
    }
    
    napi_get_boolean(env, false, &val);
    napi_set_named_property(env, result, "error", val);

    free(json_output);
    free(id);
    free(state_root);
    
    return result;
}

/**
 * list(options?) -> [{id, pid, status, bundle, created, owner}, ...]
 *
 * @param {object} [options] - تنظیمات اختیاری
 * @param {string} [options.stateRoot] - مسیر state root
 * @param {boolean} [options.systemdCgroup] - استفاده از systemd cgroup
 * @param {boolean} [options.quiet] - فقط نمایش ID ها
 */
napi_value CrunList(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1], result;
    
    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, NULL, NULL));
    
    char* state_root = NULL;
    bool systemd_cgroup = false;
    bool quiet = false;
    
    if (argc >= 1) {
        napi_valuetype type;
        napi_typeof(env, args[0], &type);
        if (type == napi_object) {
            state_root = get_string_property(env, args[0], "stateRoot");
            systemd_cgroup = get_bool_property(env, args[0], "systemdCgroup", false);
            quiet = get_bool_property(env, args[0], "quiet", false);
        }
    }

    libcrun_context_t crun_context = {0};
    
    crun_context.state_root = state_root;
    crun_context.systemd_cgroup = systemd_cgroup ? 1 : 0;
    crun_context.fifo_exec_wait_fd = -1;
    
    libcrun_error_t err = NULL;
    libcrun_container_list_t* list = NULL;
    
    const char* root = crun_context.state_root ? crun_context.state_root : "/run/crun";
    
    int ret = libcrun_get_containers_list(&list, root, &err);
    
    if (ret < 0) {
        napi_value error = create_error_from_crun(env, "Failed to list containers", &err);
        free(state_root);
        return error;
    }
    
    napi_create_array(env, &result);
    
    uint32_t index = 0;
    libcrun_container_list_t* it;
    
    for (it = list; it; it = it->next) {
        napi_value obj, val;
        napi_create_object(env, &obj);
        
        napi_create_string_utf8(env, it->name, NAPI_AUTO_LENGTH, &val);
        napi_set_named_property(env, obj, "id", val);

        if (!quiet) {
            libcrun_container_status_t status = {0};
            
            ret = libcrun_read_container_status(&status, root, it->name, &err);
            if (ret < 0) {

                if (err) libcrun_error_release(&err);
                napi_set_element(env, result, index++, obj);
                continue;
            }
            
            int running = 0;
            const char* container_status = NULL;
            
            ret = libcrun_get_container_state_string(
                it->name, &status, root, &container_status, &running, &err
            );
            
            if (ret < 0) {
                if (err) libcrun_error_release(&err);
                libcrun_free_container_status(&status);
                napi_set_element(env, result, index++, obj);
                continue;
            }
            
            int pid = running ? status.pid : 0;
            napi_create_int32(env, pid, &val);
            napi_set_named_property(env, obj, "pid", val);

            if (container_status) {
                napi_create_string_utf8(env, container_status, NAPI_AUTO_LENGTH, &val);
                napi_set_named_property(env, obj, "status", val);
            }
            
            /* bundle */
            if (status.bundle) {
                napi_create_string_utf8(env, status.bundle, NAPI_AUTO_LENGTH, &val);
                napi_set_named_property(env, obj, "bundle", val);
            }
            
            /* created */
            if (status.created) {
                napi_create_string_utf8(env, status.created, NAPI_AUTO_LENGTH, &val);
                napi_set_named_property(env, obj, "created", val);
            }
            
            /* owner */
            if (status.owner) {
                napi_create_string_utf8(env, status.owner, NAPI_AUTO_LENGTH, &val);
                napi_set_named_property(env, obj, "owner", val);
            }
            
            libcrun_free_container_status(&status);
        }
        
        napi_set_element(env, result, index++, obj);
    }
    
    libcrun_free_containers_list(list);
    free(state_root);
    
    return result;
}

/**
 * pause(id, options?) -> {id, status, error}
 * 
 * @param {string} id - شناسه کانتینر
 * @param {object} [options] - تنظیمات اختیاری
 * @param {string} [options.stateRoot] - مسیر state root (پیش‌فرض: /run/crun)
 * @param {boolean} [options.systemdCgroup] - استفاده از systemd cgroup
 */
napi_value CrunPause(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2], result;
    
    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, NULL, NULL));

    if (argc < 1) {
        return napi_create_error_obj(env, -1, "please specify a ID for the container");
    }
    
    char* id = napi_get_string(env, args[0]);
    if (!id) {
        return napi_create_error_obj(env, -1, "Invalid container id");
    }

    char* state_root = NULL;
    bool systemd_cgroup = false;
    
    if (argc >= 2) {
        napi_valuetype type;
        napi_typeof(env, args[1], &type);
        if (type == napi_object) {
            state_root = get_string_property(env, args[1], "stateRoot");
            systemd_cgroup = get_bool_property(env, args[1], "systemdCgroup", false);
        }
    }
    
    libcrun_context_t crun_context = {0};
    
    crun_context.id = id;
    crun_context.state_root = state_root;
    crun_context.systemd_cgroup = systemd_cgroup ? 1 : 0;
    crun_context.fifo_exec_wait_fd = -1;
    
    libcrun_error_t err = NULL;
    int ret = libcrun_container_pause(&crun_context, id, &err);
    
    if (ret < 0) {
        napi_value error = create_error_from_crun(env, "Failed to pause container", &err);
        free(id);
        free(state_root);
        return error;
    }
    
    napi_create_object(env, &result);
    
    napi_value val;
    
    napi_create_string_utf8(env, id, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, result, "id", val);
    
    napi_create_string_utf8(env, "paused", NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, result, "status", val);
    
    napi_get_boolean(env, false, &val);
    napi_set_named_property(env, result, "error", val);

    free(id);
    free(state_root);
    
    return result;
}


/**
 * resume(id, options?) -> {id, status, error}
 * 
 * @param {string} id - شناسه کانتینر
 * @param {object} [options] - تنظیمات اختیاری
 * @param {string} [options.stateRoot] - مسیر state root (پیش‌فرض: /run/crun)
 * @param {boolean} [options.systemdCgroup] - استفاده از systemd cgroup
 */
napi_value CrunResume(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2], result;
    
    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, NULL, NULL));
    
    if (argc < 1) {
        return napi_create_error_obj(env, -1, "please specify a ID for the container");
    }
    
    char* id = napi_get_string(env, args[0]);
    if (!id) {
        return napi_create_error_obj(env, -1, "Invalid container id");
    }

    char* state_root = NULL;
    bool systemd_cgroup = false;
    
    if (argc >= 2) {
        napi_valuetype type;
        napi_typeof(env, args[1], &type);
        if (type == napi_object) {
            state_root = get_string_property(env, args[1], "stateRoot");
            systemd_cgroup = get_bool_property(env, args[1], "systemdCgroup", false);
        }
    }

    libcrun_context_t crun_context = {0};
    
    crun_context.id = id;
    crun_context.state_root = state_root;
    crun_context.systemd_cgroup = systemd_cgroup ? 1 : 0;
    crun_context.fifo_exec_wait_fd = -1;

    libcrun_error_t err = NULL;
    int ret = libcrun_container_unpause(&crun_context, id, &err);
    
    if (ret < 0) {
        napi_value error = create_error_from_crun(env, "Failed to resume container", &err);
        free(id);
        free(state_root);
        return error;
    }

    napi_create_object(env, &result);
    
    napi_value val;
    
    napi_create_string_utf8(env, id, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, result, "id", val);
    
    napi_create_string_utf8(env, "running", NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, result, "status", val);
    
    napi_get_boolean(env, false, &val);
    napi_set_named_property(env, result, "error", val);

    free(id);
    free(state_root);
    
    return result;
}

/**
 * exec(id, command, options?) -> {id, exitCode, error}
 *
 * @param {string} id - شناسه کانتینر
 * @param {string|string[]} command - دستور (رشته یا آرایه)
 * @param {object} [options] - تنظیمات اختیاری
 * @param {string} [options.stateRoot] - مسیر state root
 * @param {boolean} [options.systemdCgroup] - استفاده از systemd cgroup
 * @param {boolean} [options.tty] - تخصیص pseudo-TTY
 * @param {boolean} [options.detach] - اجرا در پس‌زمینه
 * @param {string} [options.cwd] - دایرکتوری کاری
 * @param {string} [options.user] - کاربر به فرمت UID[:GID]
 * @param {string} [options.process] - مسیر فایل process.json
 * @param {string} [options.consoleSocket] - مسیر socket برای tty
 * @param {string} [options.pidFile] - مسیر فایل PID
 * @param {number} [options.preserveFds] - تعداد FD های اضافی
 * @param {boolean} [options.noNewPrivs] - تنظیم no new privileges
 * @param {string} [options.processLabel] - SELinux process label
 * @param {string} [options.apparmor] - AppArmor profile
 * @param {string[]} [options.env] - متغیرهای محیطی اضافی
 * @param {string[]} [options.cap] - capabilities اضافی
 * @param {string} [options.cgroup] - sub-cgroup path
 */
napi_value CrunExec(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3], result;

    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, NULL, NULL));

    if (argc < 2) {
        return napi_create_error_obj(env, -1, "please specify a ID and command for the container");
    }

    char* id = napi_get_string(env, args[0]);
    if (!id) {
        return napi_create_error_obj(env, -1, "Invalid container id");
    }

    char** cmd_args = NULL;
    size_t cmd_args_len = 0;

    napi_valuetype cmd_type;
    napi_typeof(env, args[1], &cmd_type);

    bool is_array = false;
    if (cmd_type == napi_object) {
        napi_is_array(env, args[1], &is_array);
    }

    if (is_array) {
        uint32_t arr_len;
        napi_get_array_length(env, args[1], &arr_len);

        cmd_args = (char**)calloc(arr_len + 1, sizeof(char*));
        cmd_args_len = arr_len;

        for (uint32_t i = 0; i < arr_len; i++) {
            napi_value elem;
            napi_get_element(env, args[1], i, &elem);
            cmd_args[i] = napi_get_string(env, elem);
        }
        cmd_args[arr_len] = NULL;
    } else if (cmd_type == napi_string) {
        cmd_args = (char**)calloc(2, sizeof(char*));
        cmd_args[0] = napi_get_string(env, args[1]);
        cmd_args[1] = NULL;
        cmd_args_len = 1;
    } else {
        free(id);
        return napi_create_error_obj(env, -1, "command must be a string or array of strings");
    }

    char* state_root = NULL;
    char* console_socket = NULL;
    char* pid_file = NULL;
    char* process_path = NULL;
    char* cwd = NULL;
    char* user = NULL;
    char* process_label = NULL;
    char* apparmor = NULL;
    char* cgroup = NULL;
    bool systemd_cgroup = false;
    bool tty = false;
    bool detach = false;
    bool no_new_privs = false;
    int preserve_fds = 0;

    char** extra_env = NULL;
    size_t extra_env_len = 0;
    char** extra_cap = NULL;
    size_t extra_cap_len = 0;

    if (argc >= 3) {
        napi_valuetype type;
        napi_typeof(env, args[2], &type);

        if (type == napi_object) {
            state_root = get_string_property(env, args[2], "stateRoot");
            console_socket = get_string_property(env, args[2], "consoleSocket");
            pid_file = get_string_property(env, args[2], "pidFile");
            process_path = get_string_property(env, args[2], "process");
            cwd = get_string_property(env, args[2], "cwd");
            user = get_string_property(env, args[2], "user");
            process_label = get_string_property(env, args[2], "processLabel");
            apparmor = get_string_property(env, args[2], "apparmor");
            cgroup = get_string_property(env, args[2], "cgroup");
            systemd_cgroup = get_bool_property(env, args[2], "systemdCgroup", false);
            tty = get_bool_property(env, args[2], "tty", false);
            detach = get_bool_property(env, args[2], "detach", false);
            no_new_privs = get_bool_property(env, args[2], "noNewPrivs", false);

            napi_value pfd_val;
            bool has_pfd;
            napi_has_named_property(env, args[2], "preserveFds", &has_pfd);
            if (has_pfd) {
                napi_get_named_property(env, args[2], "preserveFds", &pfd_val);
                napi_get_value_int32(env, pfd_val, &preserve_fds);
            }

            bool has_env;
            napi_has_named_property(env, args[2], "env", &has_env);
            if (has_env) {
                napi_value env_val;
                napi_get_named_property(env, args[2], "env", &env_val);
                bool env_is_array;
                napi_is_array(env, env_val, &env_is_array);
                if (env_is_array) {
                    uint32_t env_len;
                    napi_get_array_length(env, env_val, &env_len);
                    extra_env = (char**)calloc(env_len + 1, sizeof(char*));
                    extra_env_len = env_len;
                    for (uint32_t i = 0; i < env_len; i++) {
                        napi_value elem;
                        napi_get_element(env, env_val, i, &elem);
                        extra_env[i] = napi_get_string(env, elem);
                    }
                    extra_env[env_len] = NULL;
                }
            }

            bool has_cap;
            napi_has_named_property(env, args[2], "cap", &has_cap);
            if (has_cap) {
                napi_value cap_val;
                napi_get_named_property(env, args[2], "cap", &cap_val);
                bool cap_is_array;
                napi_is_array(env, cap_val, &cap_is_array);
                if (cap_is_array) {
                    uint32_t cap_len;
                    napi_get_array_length(env, cap_val, &cap_len);
                    extra_cap = (char**)calloc(cap_len + 1, sizeof(char*));
                    extra_cap_len = cap_len;
                    for (uint32_t i = 0; i < cap_len; i++) {
                        napi_value elem;
                        napi_get_element(env, cap_val, i, &elem);
                        extra_cap[i] = napi_get_string(env, elem);
                    }
                    extra_cap[cap_len] = NULL;
                }
            }
        }
    }

    libcrun_context_t crun_context = {0};

    crun_context.id = id;
    crun_context.state_root = state_root;
    crun_context.systemd_cgroup = systemd_cgroup ? 1 : 0;
    crun_context.detach = detach ? 1 : 0;
    crun_context.fifo_exec_wait_fd = -1;
    crun_context.preserve_fds = preserve_fds;
    crun_context.listen_fds = 0;

    if (console_socket)
        crun_context.console_socket = console_socket;
    if (pid_file)
        crun_context.pid_file = pid_file;

    if (getenv("LISTEN_FDS")) {
        crun_context.listen_fds = strtoll(getenv("LISTEN_FDS"), NULL, 10);
        crun_context.preserve_fds += crun_context.listen_fds;
    }

    struct libcrun_container_exec_options_s exec_opts;
    memset(&exec_opts, 0, sizeof(exec_opts));
    exec_opts.struct_size = sizeof(exec_opts);

    libcrun_error_t err = NULL;
    int ret;


    runtime_spec_schema_config_schema_process* process = NULL;

    if (process_path) {
        exec_opts.path = process_path;
    } else {
        process = (runtime_spec_schema_config_schema_process*)calloc(1, sizeof(*process));
        if (!process) {
            for (size_t i = 0; i < cmd_args_len; i++) free(cmd_args[i]);
            free(cmd_args);
            free(id); free(state_root); free(console_socket);
            free(pid_file); free(process_path); free(cwd);
            free(user); free(process_label); free(apparmor); free(cgroup);
            for (size_t i = 0; i < extra_env_len; i++) free(extra_env[i]);
            free(extra_env);
            for (size_t i = 0; i < extra_cap_len; i++) free(extra_cap[i]);
            free(extra_cap);
            return napi_create_error_obj(env, -1, "Failed to allocate process");
        }

        process->args = cmd_args;
        process->args_len = cmd_args_len;

        process->terminal = tty;

        if (cwd)
            process->cwd = cwd;

        if (extra_env) {
            process->env = extra_env;
            process->env_len = extra_env_len;
        }

        if (user) {
            runtime_spec_schema_config_schema_process_user* u =
                (runtime_spec_schema_config_schema_process_user*)calloc(1, sizeof(*u));
            if (u) {
                char* endptr = NULL;
                errno = 0;
                u->uid = strtol(user, &endptr, 10);
                if (endptr && *endptr == ':') {
                    u->gid = strtol(endptr + 1, NULL, 10);
                }
                process->user = u;
            }
        }

        if (process_label)
            process->selinux_label = process_label;

        if (apparmor)

        if (no_new_privs)
            process->no_new_privileges = 1;

        if (extra_cap && extra_cap_len > 0) {
            runtime_spec_schema_config_schema_process_capabilities* capabilities =
                (runtime_spec_schema_config_schema_process_capabilities*)calloc(1, sizeof(*capabilities));
            if (capabilities) {
                capabilities->effective = extra_cap;
                capabilities->effective_len = extra_cap_len;
                capabilities->inheritable = NULL;
                capabilities->inheritable_len = 0;
                capabilities->bounding = extra_cap;
                capabilities->bounding_len = extra_cap_len;
                capabilities->ambient = extra_cap;
                capabilities->ambient_len = extra_cap_len;
                capabilities->permitted = extra_cap;
                capabilities->permitted_len = extra_cap_len;
                process->capabilities = capabilities;
            }
        }

        exec_opts.process = process;
    }

    if (cgroup)
        exec_opts.cgroup = cgroup;


    ret = libcrun_container_exec_with_options(&crun_context, id, &exec_opts, &err);

    if (process) {
        if (process->user) free(process->user);
        if (process->capabilities) free(process->capabilities);
        free(process);
    }

    if (!process_path) {
        for (size_t i = 0; i < cmd_args_len; i++) free(cmd_args[i]);
        free(cmd_args);
    }

    if (ret < 0) {
        napi_value error = create_error_from_crun(env, "Failed to exec in container", &err);
        free(id); free(state_root); free(console_socket);
        free(pid_file); free(process_path); free(cwd);
        free(user); free(process_label); free(apparmor); free(cgroup);
        if (extra_env && !process) {
            for (size_t i = 0; i < extra_env_len; i++) free(extra_env[i]);
            free(extra_env);
        }
        if (extra_cap && (!process || !process_path)) {
            for (size_t i = 0; i < extra_cap_len; i++) free(extra_cap[i]);
            free(extra_cap);
        }
        return error;
    }

    napi_create_object(env, &result);

    napi_value val;

    napi_create_string_utf8(env, id, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, result, "id", val);

    napi_create_int32(env, ret, &val);
    napi_set_named_property(env, result, "exitCode", val);

    napi_get_boolean(env, false, &val);
    napi_set_named_property(env, result, "error", val);

    free(id); free(state_root); free(console_socket);
    free(pid_file); free(process_path); free(cwd);
    free(user); free(process_label); free(apparmor); free(cgroup);

    return result;
}

napi_value CrunSpec(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1], result;
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);
    
    bool rootless = false;
    
    if (argc >= 1) {
        napi_valuetype type;
        napi_typeof(env, args[0], &type);
        if (type == napi_object) {
            napi_value val;
            bool has_prop;
            
            napi_has_named_property(env, args[0], "rootless", &has_prop);
            if (has_prop) {
                napi_get_named_property(env, args[0], "rootless", &val);
                napi_get_value_bool(env, val, &rootless);
            }
        }
    }
    
    libcrun_error_t err = NULL;

    char* json_output = NULL;
    size_t json_size = 0;
    FILE* memstream = open_memstream(&json_output, &json_size);
    
    if (!memstream) {
        return napi_create_error_obj(env, -1, "Failed to create memory stream");
    }

    int ret = libcrun_container_spec(!rootless, memstream, &err);
    fclose(memstream);
    
    if (ret < 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Failed to generate spec: %s", 
                 err ? err->msg : "unknown error");
        if (err) libcrun_error_release(&err);
        free(json_output);
        return napi_create_error_obj(env, -1, msg);
    }
    
    napi_create_object(env, &result);
    napi_value val;

    if (json_output && json_size > 0) {
        napi_create_string_utf8(env, json_output, json_size, &val);
    } else {
        napi_create_string_utf8(env, "{}", NAPI_AUTO_LENGTH, &val);
    }
    napi_set_named_property(env, result, "spec", val);

    napi_get_boolean(env, false, &val);
    napi_set_named_property(env, result, "error", val);
    
    free(json_output);
    
    return result;
}
/**
 * update(id, resources, options?) -> {id, updated, error}
 * 
 * @param {string} id - شناسه کانتینر
 * @param {object|string} resources - منابع به صورت آبجکت یا مسیر فایل JSON
 *   اگر string باشد: مسیر فایل resources (مثل -r در crun)
 *   اگر object باشد: مقادیر مستقیم منابع
 * @param {number} [resources.blkioWeight] - وزن blkio
 * @param {number} [resources.cpuPeriod] - CPU CFS period
 * @param {number} [resources.cpuQuota] - CPU CFS quota
 * @param {number} [resources.cpuShares] - CPU shares
 * @param {number} [resources.cpuRtPeriod] - CPU realtime period
 * @param {number} [resources.cpuRtRuntime] - CPU realtime runtime
 * @param {string} [resources.cpusetCpus] - CPU(s) to use
 * @param {string} [resources.cpusetMems] - Memory node(s) to use
 * @param {number} [resources.memory] - Memory limit
 * @param {number} [resources.memoryReservation] - Memory reservation
 * @param {number} [resources.memorySwap] - Total memory usage
 * @param {number} [resources.kernelMemory] - Kernel memory limit
 * @param {number} [resources.kernelMemoryTcp] - Kernel memory TCP limit
 * @param {number} [resources.pidsLimit] - Maximum pids
 * @param {string} [resources.l3CacheSchema] - Intel RDT L3 cache schema
 * @param {string} [resources.memBwSchema] - Intel RDT memory bandwidth schema
 * @param {object} [options] - تنظیمات اختیاری
 * @param {string} [options.stateRoot] - مسیر state root
 * @param {boolean} [options.systemdCgroup] - استفاده از systemd cgroup
 */

struct update_value_entry {
    const char* section;
    const char* name;
    int numeric;
    char value_buf[64];
};

struct update_mapping {
    const char* js_prop;
    const char* section;
    const char* name;
    int numeric;
};

static const struct update_mapping update_mappings[] = {
    {"blkioWeight",     "blockIO", "weight",          1},
    {"cpuPeriod",       "cpu",     "period",          1},
    {"cpuQuota",        "cpu",     "quota",           1},
    {"cpuShares",       "cpu",     "shares",          1},
    {"cpuRtPeriod",     "cpu",     "realtimePeriod",  1},
    {"cpuRtRuntime",    "cpu",     "realtimeRuntime", 1},
    {"cpusetCpus",      "cpu",     "cpus",            0},
    {"cpusetMems",      "cpu",     "mems",            0},
    {"kernelMemory",    "memory",  "kernel",          1},
    {"kernelMemoryTcp", "memory",  "kernelTCP",       1},
    {"memory",          "memory",  "limit",           1},
    {"memoryReservation","memory", "reservation",     1},
    {"memorySwap",      "memory",  "swap",            1},
    {"pidsLimit",       "pids",    "limit",           1},
    {NULL, NULL, NULL, 0}
};

napi_value CrunUpdate(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3], result;
    
    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, NULL, NULL));
    
    if (argc < 2) {
        return napi_create_error_obj(env, -1, "please specify a ID and resources for the container");
    }
    
    char* id = napi_get_string(env, args[0]);
    if (!id) {
        return napi_create_error_obj(env, -1, "Invalid container id");
    }
    
    char* state_root = NULL;
    bool systemd_cgroup = false;
    
    if (argc >= 3) {
        napi_valuetype type;
        napi_typeof(env, args[2], &type);
        if (type == napi_object) {
            state_root = get_string_property(env, args[2], "stateRoot");
            systemd_cgroup = get_bool_property(env, args[2], "systemdCgroup", false);
        }
    }
    
    libcrun_context_t crun_context = {0};
    
    crun_context.id = id;
    crun_context.state_root = state_root;
    crun_context.systemd_cgroup = systemd_cgroup ? 1 : 0;
    crun_context.fifo_exec_wait_fd = -1;
    
    libcrun_error_t err = NULL;
    int ret;
    
    napi_valuetype res_type;
    napi_typeof(env, args[1], &res_type);

    if (res_type == napi_string) {
        char* resources_path = napi_get_string(env, args[1]);
        if (!resources_path) {
            free(id);
            free(state_root);
            return napi_create_error_obj(env, -1, "Invalid resources path");
        }
        
        ret = libcrun_container_update_from_file(&crun_context, id, resources_path, &err);
        free(resources_path);
        
        if (ret < 0) {
            napi_value error = create_error_from_crun(env, "Failed to update container from file", &err);
            free(id);
            free(state_root);
            return error;
        }
    }
    else if (res_type == napi_object) {
        struct libcrun_update_value_s* values = NULL;
        size_t values_len = 0;
        
        char value_bufs[16][64];
        int buf_idx = 0;
        
        const struct update_mapping* m;
        for (m = update_mappings; m->js_prop != NULL; m++) {
            bool has_prop;
            napi_has_named_property(env, args[1], m->js_prop, &has_prop);
            
            if (!has_prop)
                continue;
            
            napi_value prop_val;
            napi_get_named_property(env, args[1], m->js_prop, &prop_val);
            
            napi_valuetype prop_type;
            napi_typeof(env, prop_val, &prop_type);
            
            const char* value_str = NULL;
            
            if (m->numeric && prop_type == napi_number) {
                int64_t num;
                napi_get_value_int64(env, prop_val, &num);
                snprintf(value_bufs[buf_idx], sizeof(value_bufs[buf_idx]), "%ld", (long)num);
                value_str = value_bufs[buf_idx];
                buf_idx++;
            } else if (!m->numeric && prop_type == napi_string) {
                char* tmp = napi_get_string(env, prop_val);
                if (tmp && buf_idx < 16) {
                    strncpy(value_bufs[buf_idx], tmp, sizeof(value_bufs[buf_idx]) - 1);
                    value_bufs[buf_idx][sizeof(value_bufs[buf_idx]) - 1] = '\0';
                    value_str = value_bufs[buf_idx];
                    buf_idx++;
                    free(tmp);
                }
            } else if (prop_type == napi_string) {
                char* tmp = napi_get_string(env, prop_val);
                if (tmp && buf_idx < 16) {
                    strncpy(value_bufs[buf_idx], tmp, sizeof(value_bufs[buf_idx]) - 1);
                    value_bufs[buf_idx][sizeof(value_bufs[buf_idx]) - 1] = '\0';
                    value_str = value_bufs[buf_idx];
                    buf_idx++;
                    free(tmp);
                }
            }
            
            if (value_str) {
                values = realloc(values, (values_len + 1) * sizeof(struct libcrun_update_value_s));
                values[values_len].section = m->section;
                values[values_len].name = m->name;
                values[values_len].numeric = m->numeric;
                values[values_len].value = value_str;
                values_len++;
            }
        }
        
        if (values_len > 0) {
            ret = libcrun_container_update_from_values(&crun_context, id, values, values_len, &err);
            free(values);
            
            if (ret < 0) {
                napi_value error = create_error_from_crun(env, "Failed to update container", &err);
                free(id);
                free(state_root);
                return error;
            }
        }
        
        char* l3_cache = get_string_property(env, args[1], "l3CacheSchema");
        char* mem_bw = get_string_property(env, args[1], "memBwSchema");
        
        if (l3_cache || mem_bw) {
            struct libcrun_intel_rdt_update rdt_update = {
                .l3_cache_schema = l3_cache,
                .mem_bw_schema = mem_bw,
            };
            
            ret = libcrun_container_update_intel_rdt(&crun_context, id, &rdt_update, &err);
            
            if (ret < 0) {
                napi_value error = create_error_from_crun(env, "Failed to update Intel RDT", &err);
                free(l3_cache);
                free(mem_bw);
                free(id);
                free(state_root);
                return error;
            }
            
            free(l3_cache);
            free(mem_bw);
        }
    } else {
        free(id);
        free(state_root);
        return napi_create_error_obj(env, -1, "resources must be a string (file path) or object");
    }
    
    napi_create_object(env, &result);
    
    napi_value val;
    
    napi_create_string_utf8(env, id, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, result, "id", val);
    
    napi_get_boolean(env, true, &val);
    napi_set_named_property(env, result, "updated", val);
    
    napi_get_boolean(env, false, &val);
    napi_set_named_property(env, result, "error", val);
    
    free(id);
    free(state_root);
    
    return result;
}
/**
 * ps(id, options?) -> [{PID, PPID, UID, STIME, TTY, TIME, CMD, C}, ...]
 *
 * @param {string} id - شناسه کانتینر
 * @param {object} [options] - تنظیمات اختیاری
 * @param {string} [options.stateRoot] - مسیر state root
 * @param {boolean} [options.systemdCgroup] - استفاده از systemd cgroup
 */


static napi_value read_ps_info(napi_env env, int pid) {
    napi_value obj, val;
    napi_create_object(env, &obj);
    
    char path[PATH_MAX];
    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "%d", pid);

    napi_create_string_utf8(env, pid_str, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, obj, "PID", val);

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE* f = fopen(path, "r");
    if (!f) {
        napi_create_string_utf8(env, "0", NAPI_AUTO_LENGTH, &val);
        napi_set_named_property(env, obj, "PPID", val);
        napi_set_named_property(env, obj, "UID", val);
        napi_set_named_property(env, obj, "STIME", val);
        napi_set_named_property(env, obj, "TTY", val);
        napi_set_named_property(env, obj, "TIME", val);
        napi_set_named_property(env, obj, "C", val);
        napi_create_string_utf8(env, "unknown", NAPI_AUTO_LENGTH, &val);
        napi_set_named_property(env, obj, "CMD", val);
        return obj;
    }
    
    int stat_pid, ppid, pgrp, session, tty_nr;
    char comm[256], state;
    unsigned long utime, stime;
    unsigned long long starttime;

    if (fscanf(f, "%d (%255[^)]) %c %d %d %d %d",
               &stat_pid, comm, &state, &ppid, &pgrp, &session, &tty_nr) < 7) {
        fclose(f);
        napi_create_string_utf8(env, "0", NAPI_AUTO_LENGTH, &val);
        napi_set_named_property(env, obj, "PPID", val);
        napi_set_named_property(env, obj, "UID", val);
        napi_set_named_property(env, obj, "STIME", val);
        napi_set_named_property(env, obj, "TTY", val);
        napi_set_named_property(env, obj, "TIME", val);
        napi_set_named_property(env, obj, "C", val);
        char cmd_buf[260];
        snprintf(cmd_buf, sizeof(cmd_buf), "(%s)", comm);
        napi_create_string_utf8(env, cmd_buf, NAPI_AUTO_LENGTH, &val);
        napi_set_named_property(env, obj, "CMD", val);
        return obj;
    }
    
    rewind(f);
    char stat_line[4096];
    if (fgets(stat_line, sizeof(stat_line), f)) {

        char* after_comm = strchr(stat_line, ')');
        if (after_comm) {
            after_comm += 2;

            unsigned long dummy;
            sscanf(after_comm, "%c %d %d %d %d %lu %lu %lu %lu %lu %lu %lu %lu",
                   &state, &ppid, &pgrp, &session, &tty_nr,
                   &dummy, &dummy, &dummy, &dummy, &dummy, &dummy,
                   &utime, &stime);
        }
    }
    fclose(f);

    char ppid_str[32];
    snprintf(ppid_str, sizeof(ppid_str), "%d", ppid);
    napi_create_string_utf8(env, ppid_str, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, obj, "PPID", val);

    char cmd_buf[260];
    snprintf(cmd_buf, sizeof(cmd_buf), "(%s)", comm);
    napi_create_string_utf8(env, cmd_buf, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, obj, "CMD", val);

    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    f = fopen(path, "r");
    char uid_str[32] = "0";
    if (f) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "Uid:", 4) == 0) {
                int uid;
                sscanf(line + 4, "%d", &uid);
                snprintf(uid_str, sizeof(uid_str), "%d", uid);
                break;
            }
        }
        fclose(f);
    }
    napi_create_string_utf8(env, uid_str, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, obj, "UID", val);

    char stime_str[32];
    snprintf(stime_str, sizeof(stime_str), "%d", session);
    napi_create_string_utf8(env, stime_str, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, obj, "STIME", val);

    char tty_str[32];
    snprintf(tty_str, sizeof(tty_str), "%d", session);
    napi_create_string_utf8(env, tty_str, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, obj, "TTY", val);

    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    unsigned long total_time = (utime + stime) / hz;
    char time_str[32];
    snprintf(time_str, sizeof(time_str), "%lu", total_time);
    napi_create_string_utf8(env, time_str, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, obj, "TIME", val);

    napi_create_string_utf8(env, "0", NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, obj, "C", val);
    
    return obj;
}

napi_value CrunPs(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2], result;
    
    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, NULL, NULL));
    
    if (argc < 1) {
        return napi_create_error_obj(env, -1, "please specify a ID for the container");
    }
    
    char* id = napi_get_string(env, args[0]);
    if (!id) {
        return napi_create_error_obj(env, -1, "Invalid container id");
    }

    char* state_root = NULL;
    
    if (argc >= 2) {
        napi_valuetype type;
        napi_typeof(env, args[1], &type);
        if (type == napi_object) {
            state_root = get_string_property(env, args[1], "stateRoot");
        }
    }
    
    const char* root = state_root ? state_root : "/run/crun";

    libcrun_error_t err = NULL;
    libcrun_container_status_t status = {0};
    
    int ret = libcrun_read_container_status(&status, root, id, &err);
    if (ret < 0) {
        napi_value error = create_error_from_crun(env, "Failed to read container status", &err);
        free(id);
        free(state_root);
        return error;
    }
    
    int running = libcrun_is_container_running(&status, &err);
    if (running <= 0) {
        libcrun_free_container_status(&status);
        free(id);
        free(state_root);
        return napi_create_error_obj(env, -1, "container is not running");
    }
    
    int* pids = NULL;
    size_t pids_count = 0;
    
    if (status.cgroup_path) {
        char cgroup_full[PATH_MAX];
        snprintf(cgroup_full, sizeof(cgroup_full), "/sys/fs/cgroup/%s/cgroup.procs", status.cgroup_path);
        
        FILE* f = fopen(cgroup_full, "r");
        if (f) {
            char line[64];
            while (fgets(line, sizeof(line), f)) {
                int p = atoi(line);
                if (p > 0) {
                    pids = realloc(pids, (pids_count + 1) * sizeof(int));
                    pids[pids_count++] = p;
                }
            }
            fclose(f);
        }
    }
    
    if (!pids || pids_count == 0) {
        pids = realloc(pids, sizeof(int));
        pids[0] = status.pid;
        pids_count = 1;
        
        char children_path[PATH_MAX];
        snprintf(children_path, sizeof(children_path),
                 "/proc/%d/task/%d/children", status.pid, status.pid);
        
        FILE* f = fopen(children_path, "r");
        if (f) {
            int child_pid;
            while (fscanf(f, "%d", &child_pid) == 1) {
                pids = realloc(pids, (pids_count + 1) * sizeof(int));
                pids[pids_count++] = child_pid;
            }
            fclose(f);
        }
    }

    napi_create_array(env, &result);
    
    for (size_t i = 0; i < pids_count; i++) {
        napi_value proc_info = read_ps_info(env, pids[i]);
        napi_set_element(env, result, (uint32_t)i, proc_info);
    }

    free(pids);
    libcrun_free_container_status(&status);
    free(id);
    free(state_root);
    
    return result;
}

/**
 * دریافت مصرف منابع کانتینر از cgroup
 * resourceUsage(id, options?) -> {memoryStats, cpuStats, ioStats}
 */

/* خواندن محتوای یک فایل cgroup */
static char* read_cgroup_file_content(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return NULL;
    
    char* content = (char*)calloc(1, 8192);
    if (!content) { fclose(f); return NULL; }
    
    size_t n = fread(content, 1, 8191, f);
    content[n] = '\0';
    fclose(f);
    
    /* حذف newline انتها */
    if (n > 0 && content[n-1] == '\n') content[n-1] = '\0';
    
    return content;
}

/* اضافه کردن یک فایل cgroup به آبجکت */
static void add_cgroup_stat(napi_env env, napi_value obj,
                            const char* cgroup_path, const char* file,
                            const char* key) {
    char full_path[PATH_MAX];
    snprintf(full_path, sizeof(full_path), "%s%s", cgroup_path, file);
    
    char* content = read_cgroup_file_content(full_path);
    
    napi_value val;
    if (content) {
        napi_create_string_utf8(env, content, NAPI_AUTO_LENGTH, &val);
        free(content);
    } else {
        napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &val);
    }
    
    napi_set_named_property(env, obj, key, val);
}

/* جمع‌آوری Memory Stats - دقیقاً مثل collectMemoryStats */
static napi_value collect_memory_stats(napi_env env, const char* cgroup_path) {
    napi_value obj;
    napi_create_object(env, &obj);
    
    add_cgroup_stat(env, obj, cgroup_path, "/memory.current",      "Memory Current");
    add_cgroup_stat(env, obj, cgroup_path, "/memory.max",          "Memory Max");
    add_cgroup_stat(env, obj, cgroup_path, "/memory.high",         "Memory High");
    add_cgroup_stat(env, obj, cgroup_path, "/memory.swap.max",     "Memory Swap Max");
    add_cgroup_stat(env, obj, cgroup_path, "/memory.swap.high",    "Memory Swap High");
    add_cgroup_stat(env, obj, cgroup_path, "/memory.swap.current", "Memory Swap Current");
    add_cgroup_stat(env, obj, cgroup_path, "/memory.low",          "Memory Low");
    add_cgroup_stat(env, obj, cgroup_path, "/memory.min",          "Memory Min");
    
    return obj;
}

/* جمع‌آوری CPU Stats - دقیقاً مثل collectCpuStats */
static napi_value collect_cpu_stats(napi_env env, const char* cgroup_path) {
    napi_value obj;
    napi_create_object(env, &obj);
    
    add_cgroup_stat(env, obj, cgroup_path, "/cpu.stat",                "CPU Usage");
    add_cgroup_stat(env, obj, cgroup_path, "/cpu.pressure",            "CPU Pressure");
    add_cgroup_stat(env, obj, cgroup_path, "/cpu.max",                 "CPU Max");
    add_cgroup_stat(env, obj, cgroup_path, "/cpu.weight",              "CPU Weight");
    add_cgroup_stat(env, obj, cgroup_path, "/cpu.weight.nice",         "CPU Weight Nice");
    add_cgroup_stat(env, obj, cgroup_path, "/cpu.uclamp.min",          "CPU Uclamp Min");
    add_cgroup_stat(env, obj, cgroup_path, "/cpu.uclamp.max",          "CPU Uclamp Max");
    add_cgroup_stat(env, obj, cgroup_path, "/cpuset.cpus.effective",   "Effective CPUs");
    add_cgroup_stat(env, obj, cgroup_path, "/cpuset.cpus.exclusive",   "Exclusive CPUs");
    add_cgroup_stat(env, obj, cgroup_path, "/cpuset.cpus.partition",   "Partition CPUs");
    
    return obj;
}

/* جمع‌آوری IO Stats - دقیقاً مثل collectIoStats */
static napi_value collect_io_stats(napi_env env, const char* cgroup_path) {
    napi_value obj;
    napi_create_object(env, &obj);
    
    add_cgroup_stat(env, obj, cgroup_path, "/io.max",        "IO Max");
    add_cgroup_stat(env, obj, cgroup_path, "/io.pressure",   "IO Pressure");
    add_cgroup_stat(env, obj, cgroup_path, "/io.prio.class", "IO Priority Class");
    add_cgroup_stat(env, obj, cgroup_path, "/io.stat",       "IO Stat");
    add_cgroup_stat(env, obj, cgroup_path, "/io.weight",     "IO Weight");
    
    return obj;
}

napi_value CrunResourceUsage(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2], result;
    
    NAPI_CALL(env, napi_get_cb_info(env, info, &argc, args, NULL, NULL));
    
    if (argc < 1) {
        return napi_create_error_obj(env, -1, "please specify a ID for the container");
    }
    
    char* id = napi_get_string(env, args[0]);
    if (!id) {
        return napi_create_error_obj(env, -1, "Invalid container id");
    }
    
    /* خواندن options */
    char* state_root = NULL;
    
    if (argc >= 2) {
        napi_valuetype type;
        napi_typeof(env, args[1], &type);
        if (type == napi_object) {
            state_root = get_string_property(env, args[1], "stateRoot");
        }
    }
    
    const char* root = state_root ? state_root : "/run/crun";
    
    /* خواندن وضعیت کانتینر برای پیدا کردن cgroup_path */
    libcrun_error_t err = NULL;
    libcrun_container_status_t status = {0};
    
    int ret = libcrun_read_container_status(&status, root, id, &err);
    if (ret < 0) {
        napi_value error = create_error_from_crun(env, "Failed to read container status", &err);
        free(id);
        free(state_root);
        return error;
    }
    
    /* بررسی اینکه کانتینر در حال اجراست */
    int running = libcrun_is_container_running(&status, &err);
    if (running <= 0) {
        libcrun_free_container_status(&status);
        free(id);
        free(state_root);
        return napi_create_error_obj(env, -1, "container is not running");
    }
    
    /* ساخت مسیر cgroup */
    char cgroup_path[PATH_MAX];
    cgroup_path[0] = '\0';
    
    if (status.cgroup_path) {
        snprintf(cgroup_path, sizeof(cgroup_path), "/sys/fs/cgroup/%s", status.cgroup_path);
    } else {
        /* fallback: خواندن از /proc/[pid]/cgroup */
        char proc_cgroup[PATH_MAX];
        snprintf(proc_cgroup, sizeof(proc_cgroup), "/proc/%d/cgroup", status.pid);
        
        FILE* f = fopen(proc_cgroup, "r");
        if (f) {
            char line[1024];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "0::", 3) == 0) {
                    char* path = line + 3;
                    size_t len = strlen(path);
                    if (len > 0 && path[len-1] == '\n') path[len-1] = '\0';
                    snprintf(cgroup_path, sizeof(cgroup_path), "/sys/fs/cgroup%s", path);
                    break;
                }
            }
            fclose(f);
        }
    }
    
    if (cgroup_path[0] == '\0') {
        libcrun_free_container_status(&status);
        free(id);
        free(state_root);
        return napi_create_error_obj(env, -1, "Cannot find cgroup path");
    }
    
    /* ساخت آبجکت نتیجه */
    napi_create_object(env, &result);
    
    /* جمع‌آوری آمار - دقیقاً مثل کد اصلی */
    napi_value memory_stats = collect_memory_stats(env, cgroup_path);
    napi_set_named_property(env, result, "memoryStats", memory_stats);
    
    napi_value cpu_stats = collect_cpu_stats(env, cgroup_path);
    napi_set_named_property(env, result, "cpuStats", cpu_stats);
    
    napi_value io_stats = collect_io_stats(env, cgroup_path);
    napi_set_named_property(env, result, "ioStats", io_stats);
    
    /* آزادسازی */
    libcrun_free_container_status(&status);
    free(id);
    free(state_root);
    
    return result;
}

napi_value Init(napi_env env, napi_value exports) {

    napi_property_descriptor props[] = {
        {"create", NULL, CrunCreate, NULL, NULL, NULL, napi_default, NULL},
        {"start", NULL, CrunStart, NULL, NULL, NULL, napi_default, NULL},
        {"run", NULL, CrunRun, NULL, NULL, NULL, napi_default, NULL},
        {"kill", NULL, CrunKill, NULL, NULL, NULL, napi_default, NULL},
        {"delete", NULL, CrunDelete, NULL, NULL, NULL, napi_default, NULL},
        {"state", NULL, CrunState, NULL, NULL, NULL, napi_default, NULL},
        {"list", NULL, CrunList, NULL, NULL, NULL, napi_default, NULL},
        {"pause", NULL, CrunPause, NULL, NULL, NULL, napi_default, NULL},
        {"resume", NULL, CrunResume, NULL, NULL, NULL, napi_default, NULL},
        {"exec", NULL, CrunExec, NULL, NULL, NULL, napi_default, NULL},
        {"spec", NULL, CrunSpec, NULL, NULL, NULL, napi_default, NULL},
        {"update", NULL, CrunUpdate, NULL, NULL, NULL, napi_default, NULL},
        {"ps",      NULL, CrunPs,     NULL, NULL, NULL, napi_default, NULL},
        {"resourceUsage", NULL, CrunResourceUsage, NULL, NULL, NULL, napi_default, NULL},
    };
    
    NAPI_CALL(env, napi_define_properties(env, exports, sizeof(props) / sizeof(props[0]), props));
    
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)