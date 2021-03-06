#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <zookeeper.h>

#include <string>

#include "qlibc.h"
#include "qconf_zk.h"
#include "qconf_log.h"
#include "qconf_const.h"
#include "qconf_config.h"

using namespace std;

static FILE *_zoo_log_fp = NULL;

static int zk_get_service_status(zhandle_t *zh, const string &path, char &status);
static int children_node_cmp(const void* p1, const void* p2);

/**
 * Get znode from zookeeper, and set a watcher
 */
int zk_get_node(zhandle_t *zh, const string &path, string &buf)
{
    int ret = 0;
    char buffer[QCONF_MAX_VALUE_SIZE];
    int buffer_len = QCONF_MAX_VALUE_SIZE;

    for (int i = 0; i < QCONF_GET_RETRIES; ++i)
    {
        ret = zoo_get(zh, path.c_str(), 1, buffer, &buffer_len, NULL);
        switch (ret)
        {
            case ZOK:
                if (-1 == buffer_len) buffer_len = 0;
                buf.assign(buffer, buffer_len);
                return QCONF_OK;
            case ZNONODE:
                LOG_ERR("Node not exist on zookeeper. err:%s. path:%s", 
                        zerror(ret), path.c_str());
                return QCONF_NODE_NOT_EXIST;
            case ZINVALIDSTATE:
            case ZMARSHALLINGERROR:
                continue;
            default:
                LOG_ERR("Failed to call zoo_get. err:%s. path:%s", 
                        zerror(ret), path.c_str());
                return QCONF_ERR_ZOO_FAILED;
        }
    }

    LOG_ERR("Failed to call zoo_get after retry. err:%s. path:%s", 
            zerror(ret), path.c_str());
    return QCONF_ERR_ZOO_FAILED;
}

/**
 * Get children nodes from zookeeper and set a watcher
 */
int zk_get_chdnodes(zhandle_t *zh, const string &path, string_vector_t &nodes)
{
    if (NULL == zh || path.empty()) return QCONF_ERR_PARAM;

    int ret;
    for (int i = 0; i < QCONF_GET_RETRIES; ++i)
    {
        ret = zoo_get_children(zh, path.c_str(), 1, &nodes);
        switch(ret)
        {
            case ZOK:
                qsort(nodes.data, nodes.count, sizeof(char*), children_node_cmp);
                return QCONF_OK;
            case ZNONODE:
                LOG_ERR("Node not exist on zookeeper. err:%s. path:%s",
                        zerror(ret), path.c_str());
                return QCONF_NODE_NOT_EXIST;
            case ZINVALIDSTATE:
            case ZMARSHALLINGERROR:
                continue;
            default:
                LOG_ERR("Failed to call zoo_get_children. err:%s. path:%s",
                        zerror(ret), path.c_str());
                return QCONF_ERR_ZOO_FAILED;
        }
    }

    LOG_ERR("Failed to call zoo_get_children after retry. err:%s. path:%s",
            zerror(ret), path.c_str());
    return QCONF_ERR_ZOO_FAILED;
}

int zk_get_chdnodes_with_status(zhandle_t *zh, const string &path, string_vector_t &nodes, vector<char> &status)
{
    if (NULL == zh || path.empty()) return QCONF_ERR_PARAM;
    int ret = zk_get_chdnodes(zh, path, nodes);
    if (QCONF_OK == ret)
    {
        string child_path;
        status.resize(nodes.count);
        for (int i = 0; i < nodes.count; ++i)
        {
            child_path = path + '/' + nodes.data[i];
            char s = 0;
            ret = zk_get_service_status(zh, child_path, s);
            if (QCONF_OK != ret) return QCONF_ERR_OTHER;
            status[i] = s;
        }
    }
    return ret;
}

static int zk_get_service_status(zhandle_t *zh, const string &path, char &status)
{
    if (NULL == zh || path.empty()) return QCONF_ERR_PARAM;

    string buf;
    if (QCONF_OK == zk_get_node(zh, path, buf))
    {
        long value = STATUS_UNKNOWN;
        get_integer(buf, value);
        switch(value)
        {
            case STATUS_UP:
            case STATUS_DOWN:
            case STATUS_OFFLINE:
                status = static_cast<char>(value);
                break;
            default:          
                LOG_FATAL_ERR("Invalid service status of path:%s, status:%ld!",
                        path.c_str(), value);
                return QCONF_ERR_OTHER;
        }
    }
    else
    {
        LOG_ERR( "Failed to get service status, path:%s", path.c_str());
        return QCONF_ERR_OTHER;
    }
    return QCONF_OK;
}

static int children_node_cmp(const void* p1, const void* p2)
{
    char **s1 = (char**)p1;
    char **s2 = (char**)p2;

    return strcmp(*s1, *s2);
}

int zk_register_ephemeral(zhandle_t *zh, const string &path, const string &value)
{
    if (NULL == zh || path.empty() || value.empty()) return QCONF_ERR_PARAM;

    string cur_path;
    size_t pos  = path.find_first_of('/', 1);
    while (string::npos != pos)
    {
        cur_path = path.substr(0, pos);
        int ret = zoo_create(zh, cur_path.c_str(), NULL, 0, &ZOO_OPEN_ACL_UNSAFE, 0, NULL, 0);
        if (ZOK != ret && ZNODEEXISTS != ret)
        {
            LOG_ERR("Failed register ephemeral node:%s!", cur_path.c_str());
            return QCONF_ERR_ZOO_FAILED;
        }
        pos = path.find_first_of('/', pos + 1);
    }

    switch (zoo_create(zh, path.c_str(), value.c_str(), value.size(), &ZOO_OPEN_ACL_UNSAFE, ZOO_EPHEMERAL, NULL, 0))
    {
        case ZNODEEXISTS:
            LOG_INFO("Ephemeral node:%s alread EXIST!", path.c_str());
        case ZOK:
            return QCONF_OK;
        default:
            LOG_ERR("Failed to register ephemeral node:%s!", path.c_str());
            return QCONF_ERR_ZOO_FAILED;
    }
}

int qconf_init_zoo_log(const string &log_dir, const string &zoo_log)
{
    if (log_dir.empty() || zoo_log.empty()) return QCONF_ERR_PARAM;

    string log_path = log_dir + "/" + zoo_log;
    umask(0);
    _zoo_log_fp = fopen(log_path.c_str(), "a+");
    if (NULL == _zoo_log_fp)
    {
        LOG_ERR("Failed to open zoo log file:%s, errno:%d", log_path.c_str(), errno);
        return QCONF_ERR_FAILED_OPEN_FILE;
    }
    zoo_set_log_stream(_zoo_log_fp);
    zoo_set_debug_level(ZOO_LOG_LEVEL_WARN);
    return QCONF_OK;
}

void qconf_destroy_zoo_log()
{
    if (_zoo_log_fp != NULL)
    {
        fclose(_zoo_log_fp);
        _zoo_log_fp = NULL;
    }
}
