#ifndef LMSPI_H
#define LMSPI_H

#include "lmice_trace.h"
#include "udss.h"

#include <string>
#include <map>

#include <stdarg.h>

#include <iconv.h>
#include <unistd.h>
#include <sys/types.h>


#define EXCHANGE_ID "SHFE"

/**
 * config: 开平模式，在config中设置
 *
 */

typedef void(*symbol_callback)(const char* symbol, const void* addr, uint32_t size);

class CLMSpi
{
public:
    CLMSpi(const char* name="Model");
    ~CLMSpi();

    void logging(const char* format, ...);

    std::string gbktoutf8(char *pgbk);

    void subscribe(const char* symbol);

    void unsubscribe(const char* symbol);

    void publish(const char* symbol);

    void send(const char* symbol, const void* addr, int len);

    /** dir: 0:buy  1:sell
     * return: requestId
     *
     *  开 平:后台维护
     * 增加一个字段
     * （time，order, volumn)组合
     */
    int order(const char* symbol, int dir, double price, int num);

    /**
     * @brief cancel
     * @param requestId: order return requestId
     * @param sysId: system return Id
     * return requestId
     */
    int cancel(int requestId, int sysId = 0);

    int register_callback(symbol_callback func);

    /* order tracker */
//    int get_order(const char* order_id, struct order_t * order);
    /* P & L tracker */
    /* position tracker */
private:
	void logging_bson_order( void *ptrOrd );
	void logging_bson_cancel( void *ptrOrd );

private:
    std::string m_name;
    uds_msg *sid;
    lmice_trace_info_t m_info;
//    sub_data_t* m_sub;
    std::map<std::string, void*> m_shms;
    void* m_priv;
};

#endif // LMSPI_H
