#include "lmspi.h"
#include "fmspi.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>

#include <iconv.h>
#include <unistd.h>
#include <sys/types.h>

#include "lmice_eal_bson.h"

#include "lmice_eal_shm.h"
#include "lmice_eal_event.h"
#include "lmice_eal_hash.h"
#include "lmice_eal_spinlock.h"


struct spi_private {
    pthread_t pt;
    volatile int quit_flag;
    lmice_event_t event;
    lmice_shm_t board;
    uint32_t shmcount;
    pubsub_shm_t shmlist[256];

    void(*callback)(const char* symbol, const void* addr);


};

#define SOCK_FILE "/var/run/lmiced.socket"

#define OPT_LOG_DEBUG 1

volatile int quit_flag = 0;

void spi_signal_handler(int sig) {
    if(sig == SIGTERM || sig == SIGINT)
        quit_flag = 1;
}


void* spi_thread(void* priv) {
    spi_private* p = (spi_private*) priv;
    uint32_t cnt;
    char* symlist = new char[SYMBOL_LENGTH*CLIENT_SPCNT];
    memset(symlist, 0, SYMBOL_LENGTH*CLIENT_SPCNT);
    for(;;) {
        int ret = eal_event_wait_timed(p->event.fd, 500);
        if(ret == -1) {
            /* Timed out or interrupted */
        } else if(ret == 0) {
            /* Event fired */
            sub_data_t* dt = (sub_data_t*)((char*)p->board.addr + CLIENT_SUBPOS);

            /* Copy event */
            eal_spin_lock(&dt->lock);

            cnt = dt->count;
            if(cnt <= CLIENT_SPCNT) {
                memcpy(symlist, dt->symbol, cnt*SYMBOL_LENGTH);
            } else {
                cnt = 0;
            }
            dt->count = 0;

            eal_spin_unlock(&dt->lock);

            /* Callback event */
            for(size_t i=0; i<cnt; ++i) {
                const char* symbol = dt->symbol + i*SYMBOL_LENGTH;
                uint64_t hval;
                char name[SYMBOL_LENGTH] = {0};
                hval = eal_hash64_fnv1a(symbol, strlen(symbol));
                eal_shm_hash_name(hval, name);
                for(size_t j=0; j<p->shmcount; ++j) {
                    pubsub_shm_t* ps = &p->shmlist[j];
                    lmice_shm_t* shm = &ps->shm;
                    if(memcmp(shm->name, name, SYMBOL_LENGTH) == 0 &&
                            ps->type == CLIENT_SUBSYM) {
                        if(shm->fd == 0) {
                            /* open shm */
                            eal_shm_open_readonly(shm);
                        }
                        p->callback(symbol, shm->addr);
                        break;
                    }
                }
            }
        } /* else-if ret */

        if(quit_flag == 1)
            break;
        if(p->quit_flag == 1)
            break;
    }/* end-for: ;;*/

    delete[] symlist;
    return NULL;
}

CLMSpi::CLMSpi(const char *name)
{
    m_name = name;
    create_uds_msg((void**)&sid);
    init_uds_client(SOCK_FILE, sid);
    memset(&m_info, 0, sizeof(m_info));
    m_info.pid = getpid();
    m_info.tid = eal_gettid();
    m_info.type = LMICE_TRACE_TYPE;
    lmice_trace_info_t* pinfo = (lmice_trace_info_t*)sid->data;
    pinfo->loglevel=0;
    pinfo->pid = m_info.pid;
    pinfo->tid = m_info.tid;
    time(&pinfo->tm);
    get_system_time(&pinfo->systime);

    // Register signal handler
    signal(SIGINT, spi_signal_handler);
    /*signal(SIGCHLD,SIG_IGN);  ignore child */
    /* signal(SIGTSTP,SIG_IGN);  ignore tty signals */
    signal(SIGTERM,spi_signal_handler); /* catch kill signal */

    //Register client
    pinfo->type = EM_LMICE_REGCLIENT_TYPE;
    sid->size = sizeof(lmice_trace_info_t);
    int ret = send_uds_msg(sid);
    if(ret == sid->size ) {
        struct sockaddr_un addr;
        socklen_t addr_len;
        lmice_critical_print("Register model[%s] ...\n", name);
        sid->size = recvfrom(sid->sock, sid->data, sizeof sid->data, 0, (struct sockaddr*)&(addr), &addr_len);
        lmice_critical_print("Registered model[%s] as [%d]\n", name, *((int*)sid->data));
    }

    logging("LMice client[ %s ] running %d:%d", m_name.c_str(), getuid(), getpid());

    // create thread
    spi_private* p = new spi_private;
    memset(p, 0, sizeof(spi_private));

    uint64_t hval;
    hval = eal_hash64_fnv1a(sid->local_un.sun_path, strlen(sid->local_un.sun_path));

    eal_shm_hash_name(hval, p->board.name);
    eal_shm_open_readwrite(&p->board);

    eal_event_hash_name(hval, p->event.name);
    eal_event_open(&p->event);

    m_priv = (void*) p;
    pthread_create(&p->pt, NULL, spi_thread, m_priv);

}

CLMSpi::~CLMSpi()
{
    logging("LMice %s stopped.\n\n", m_name.c_str());
    finit_uds_msg(sid);
}

void CLMSpi::logging(const char* format, ...) {
    va_list argptr;

    time(&m_info.tm);
    get_system_time(&m_info.systime);
    memset(sid->data, 0, sizeof(sid->data));
    memcpy(sid->data, &m_info, sizeof(m_info) );
    sid->size = sizeof(m_info);

    va_start(argptr, format);
    sid->size += vsprintf(sid->data+sizeof(m_info), format, argptr);
    va_end(argptr);

    send_uds_msg(sid);
}

void CLMSpi::logging_bson_order( void *ptrOrd )
{
	if( NULL == ptrOrd )
	{
		return;
	}
	
	CUstpFtdcInputOrderField *ord = (CUstpFtdcInputOrderField *)ptrOrd;
    lmice_trace_bson_info_t *pinfo = (lmice_trace_bson_info_t *)sid->data;
	EalBson bson;
    time(&pinfo->tm);
    get_system_time(&pinfo->systime);
	strcpy(pinfo->model_name, m_name.c_str());
    pinfo->type = EMZ_LMICE_TRACEZ_BSON_TYPE;

	bson.AppendTimeT("time", pinfo->tm);
	bson.AppendSymbol("BrokerID", ord->BrokerID);
	bson.AppendSymbol("ExchangeID", ord->ExchangeID);
	bson.AppendSymbol("OrderSysID", ord->OrderSysID);
	bson.AppendSymbol("InvestorID", ord->InvestorID);
	bson.AppendSymbol("UserID", ord->UserID);
	bson.AppendSymbol("InstrumentID", ord->InstrumentID);
	bson.AppendSymbol("UserOrderLocalID", ord->UserOrderLocalID);
	bson.AppendFlag("OrderPriceType", ord->OrderPriceType);
	bson.AppendFlag("Direction", ord->Direction);
	bson.AppendFlag("OffsetFlag", ord->OffsetFlag);
	bson.AppendFlag("HedgeFlag", ord->HedgeFlag);
	bson.AppendDouble("LimitPrice",ord->LimitPrice);
	bson.AppendInt64("Volume",ord->Volume);
	bson.AppendFlag("TimeCondition", ord->TimeCondition);
	bson.AppendSymbol("GTDDate", ord->GTDDate);
	bson.AppendFlag("VolumeCondition", ord->VolumeCondition);
	bson.AppendInt64("MinVolume",ord->MinVolume);
	bson.AppendDouble("StopPrice",ord->StopPrice);
	bson.AppendFlag("ForceCloseReason", ord->ForceCloseReason);
	bson.AppendInt64("MinVolume",ord->MinVolume);
	bson.AppendInt64("IsAutoSuspend",ord->IsAutoSuspend);
	bson.AppendSymbol("BusinessUnit", ord->BusinessUnit);
	bson.AppendSymbol("UserCustom", ord->UserCustom);
	bson.AppendInt64("BusinessLocalID",ord->BusinessLocalID);
	bson.AppendSymbol("ActionDay", ord->ActionDay);
	memcpy( sid->data+sizeof(m_info), bson.GetBsonData(), bson.GetLen());

    send_uds_msg(sid);
}

void CLMSpi::logging_bson_cancel( void *ptrOrd )
{
	if( NULL == ptrOrd )
	{
		return;
	}

	CUstpFtdcOrderActionField *ord = (CUstpFtdcOrderActionField *)ptrOrd;
	
    lmice_trace_bson_info_t *pinfo = (lmice_trace_bson_info_t *)sid->data;
	EalBson bson;
    get_system_time(&pinfo->systime);
	strcpy(pinfo->model_name, m_name.c_str());
    pinfo->type = EMZ_LMICE_TRACEZ_BSON_TYPE;

	bson.AppendTimeT("time", pinfo->tm);
	bson.AppendSymbol("BrokerID", ord->BrokerID);
	bson.AppendSymbol("ExchangeID", ord->ExchangeID);
	bson.AppendSymbol("OrderSysID", ord->OrderSysID);
	bson.AppendSymbol("InvestorID", ord->InvestorID);
	bson.AppendSymbol("UserID", ord->UserID);
	bson.AppendSymbol("UserOrderLocalID", ord->UserOrderLocalID);
	bson.AppendSymbol("UserOrderActionLocalID", ord->UserOrderActionLocalID);
	bson.AppendFlag("ActionFlag", ord->ActionFlag);
	bson.AppendDouble("LimitPrice",ord->LimitPrice);
	bson.AppendInt64("VolumeChange",ord->VolumeChange);
	bson.AppendInt64("BusinessLocalID",ord->BusinessLocalID);
	memcpy( sid->data+sizeof(m_info), bson.GetBsonData(), bson.GetLen());

    send_uds_msg(sid);
}

static int code_convert(const char *from_charset,const char *to_charset,char *inbuf,size_t inlen,char *outbuf,size_t outlen)
{
        iconv_t cd;
        int rc;
        char **pin = &inbuf;
        char **pout = &outbuf;

        cd = iconv_open(to_charset,from_charset);
        if (cd==0)
                return -1;
        memset(outbuf,0,outlen);
        if (iconv(cd,pin,&inlen,pout,&outlen) == (size_t)-1)
                return -1;
        iconv_close(cd);
        return 0;
}

std::string CLMSpi::gbktoutf8( char *pgbk)
{
    size_t inlen = strlen(pgbk);
    char outbuf[512] ={0};
    size_t outlen = 512;
    code_convert("gb2312","utf-8",pgbk,inlen,(char*)outbuf,outlen);

    std::string utf8;
    utf8.insert(utf8.begin(), outbuf, outbuf+strlen(outbuf));
    return utf8;

}

void CLMSpi::subscribe(const char *symbol)
{

    spi_private* p =(spi_private*)m_priv;

    if(p->shmcount >= CLIENT_SPCNT) {
        lmice_error_print("Sub/pub resource is full\n");
        return;
    }
    if(strlen(symbol) >= SYMBOL_LENGTH) {
        lmice_error_print("Sub symbol is too long.\n");
        return;
    }

    char name[SYMBOL_LENGTH] ={0};
    uint64_t hval;
    hval = eal_hash64_fnv1a(symbol, strlen(symbol));
    eal_shm_hash_name(hval, name);

    pubsub_shm_t*ps;
    lmice_shm_t*shm;
    for(size_t i=0; i< p->shmcount; ++i) {
        ps = &p->shmlist[i];
        shm = &ps->shm;
        if(ps->type == CLIENT_SUBSYM && memcmp(shm->name, name, SYMBOL_LENGTH) == 0) {
            lmice_critical_print("Already subscribed resource[%s]\n", symbol);
            return;
        }

    }
    ps = &p->shmlist[p->shmcount];
    shm = &ps->shm;
    ps->type = CLIENT_SUBSYM;
    memset(shm, 0, sizeof(lmice_shm_t) );
    memcpy(shm->name, name, SYMBOL_LENGTH);


    sid->size = sizeof(lmice_sub_t);
    lmice_sub_t *psub = (lmice_sub_t *)sid->data;
    lmice_trace_info_t *pinfo = &psub->info;
    time(&pinfo->tm);
    get_system_time(&pinfo->systime);
    pinfo->type = EM_LMICE_SUB_TYPE;

    memset(psub->symbol, 0, SYMBOL_LENGTH);
    strncpy(psub->symbol, symbol, strlen(symbol));

    send_uds_msg(sid);


}

void CLMSpi::unsubscribe(const char *symbol)
{
    spi_private* p =(spi_private*)m_priv;

    if(p->shmcount == 0) {
        lmice_error_print("Sub/pub resource is empty.\n");
        return;
    }
    if(strlen(symbol) >= SYMBOL_LENGTH) {
        lmice_error_print("Sub symbol is too long.\n");
        return;
    }

    char name[SYMBOL_LENGTH] ={0};
    uint64_t hval;
    hval = eal_hash64_fnv1a(symbol, strlen(symbol));
    eal_shm_hash_name(hval, name);

    pubsub_shm_t*ps;
    lmice_shm_t*shm;
    for(size_t i=0; i< p->shmcount; ++i) {
        ps = &p->shmlist[i];
        shm = &ps->shm;
        if(ps->type == CLIENT_SUBSYM && memcmp(shm->name, name, SYMBOL_LENGTH) == 0) {
            if(shm->fd != 0) {
                /* Close shm */
                eal_shm_close(shm->fd,shm->addr);
            }
        }

    }

    lmice_unsub_t *psub = (lmice_unsub_t *)sid->data;
    lmice_trace_info_t *pinfo = &psub->info;

    time(&pinfo->tm);
    get_system_time(&pinfo->systime);
    pinfo->type = EM_LMICE_UNSUB_TYPE;
    strncpy(psub->symbol, symbol, strlen(symbol)>sizeof(psub->symbol)?sizeof(psub->symbol):strlen(symbol));

    send_uds_msg(sid);
}

int CLMSpi::cancel(int requestId, int sysId)
{
    /*FIXME: autoincrease */
    int req = 0;
    CTraderSpi* spi = static_cast<CTraderSpi*>(this);
    CUstpFtdcOrderActionField ord;
    memset(&ord,0, sizeof(ord));

    requestId = spi->GetRequestID();
    ///交易所代码
    memcpy(ord.ExchangeID, EXCHANGE_ID, sizeof(EXCHANGE_ID)-1 );
    ///报单编号 优先级最高，填写系统返回单号，撤报单
    if(sysId) {
        sprintf(ord.OrderSysID, "%012d", sysId);
    }

    ///经纪公司编号
    memcpy(ord.BrokerID, spi->GetConf()->g_BrokerID, sizeof(ord.BrokerID) );
    ///投资者编号
    memcpy(ord.InvestorID, spi->GetConf()->g_InvestorID, sizeof(ord.InvestorID) );
    ///用户代码
    memcpy(ord.UserID, spi->GetConf()->g_UserID, sizeof(ord.UserID) );
    ///本次撤单操作的本地编号
    //TUstpFtdcUserOrderLocalIDType	UserOrderActionLocalID;
    sprintf(ord.UserOrderActionLocalID, "%012d",req);

    ///被撤订单的本地报单编号
//    TUstpFtdcUserOrderLocalIDType	UserOrderLocalID;
    sprintf(ord.UserOrderLocalID, "%012d", requestId);
    ///报单操作标志 ‘0’
//    TUstpFtdcActionFlagType	ActionFlag;
    ord.ActionFlag =USTP_FTDC_AF_Delete;
    /// 下面字段可空
    ///价格
    //TUstpFtdcPriceType	LimitPrice;
    ///数量变化
    //TUstpFtdcVolumeType	VolumeChange;
    ///本地业务标识
    //TUstpFtdcBusinessLocalIDType	BusinessLocalID;

    /// 发出撤单操作
    spi->GetTrader()->ReqOrderAction(&ord, req);

#ifdef OPT_LOG_DEBUG
	int64_t systime = 0;
	get_system_time(&systime);
	EalBson bson;
	bson.AppendInt64("time", systime);
	bson.AppendSymbol("BrokerID", ord.BrokerID);
	bson.AppendSymbol("ExchangeID", ord.ExchangeID);
	bson.AppendSymbol("OrderSysID", ord.OrderSysID);
	bson.AppendSymbol("InvestorID", ord.InvestorID);
	bson.AppendSymbol("UserID", ord.UserID);
	bson.AppendSymbol("UserOrderLocalID", ord.UserOrderLocalID);
	bson.AppendSymbol("UserOrderActionLocalID", ord.UserOrderActionLocalID);
	bson.AppendFlag("ActionFlag", ord.ActionFlag);
	bson.AppendDouble("LimitPrice",ord.LimitPrice);
	bson.AppendInt64("VolumeChange",ord.VolumeChange);
	bson.AppendInt64("BusinessLocalID",ord.BusinessLocalID);

	const char *strJson = bson.GetJsonData();

	logging("[future opt] send ( cancel ), content: %s", strJson);

	bson.FreeJsonData();
#endif

	//logging_bson_cancel( (void *)&ord );

    return req;

}

int CLMSpi::order(const char *symbol, int dir, double price, int num)
{
    CTraderSpi* spi = static_cast<CTraderSpi*>(this);
    CUstpFtdcInputOrderField ord;
    memset(&ord, 0, sizeof(ord));

    ///经纪公司编号
    TUstpFtdcBrokerIDType	BrokerID;
    ///交易所代码
    TUstpFtdcExchangeIDType	ExchangeID;
    ///系统报单编号 。置空
    TUstpFtdcOrderSysIDType	OrderSysID;
    ///投资者编号
    TUstpFtdcInvestorIDType	InvestorID;
    ///用户代码
    TUstpFtdcUserIDType	UserID;
    ///合约代码
    TUstpFtdcInstrumentIDType	InstrumentID;
    ///用户本地报单号 。用户自己维护，用来跟踪报单状态 integer,  递增，%012d
    TUstpFtdcUserOrderLocalIDType	UserOrderLocalID;
    ///报单类型
    TUstpFtdcOrderPriceTypeType	OrderPriceType;
    ///买卖方向
    TUstpFtdcDirectionType	Direction;
    ///开平标志
    TUstpFtdcOffsetFlagType	OffsetFlag;
    ///投机套保标志
    TUstpFtdcHedgeFlagType	HedgeFlag;
    ///价格
    TUstpFtdcPriceType	LimitPrice;
    ///数量
    TUstpFtdcVolumeType	Volume;
    ///有效期类型
    TUstpFtdcTimeConditionType	TimeCondition;
    ///GTD日期 yyyymmdd
    TUstpFtdcDateType	GTDDate;
    ///成交量类型
    TUstpFtdcVolumeConditionType	VolumeCondition;
    ///最小成交量
    TUstpFtdcVolumeType	MinVolume;
    ///止损价
    TUstpFtdcPriceType	StopPrice;
    ///强平原因
    TUstpFtdcForceCloseReasonType	ForceCloseReason;
    ///自动挂起标志
    TUstpFtdcBoolType	IsAutoSuspend;
    ///业务单元 。没有用
    TUstpFtdcBusinessUnitType	BusinessUnit;
    ///用户自定义域 。服务器原样返回
    TUstpFtdcCustomType	UserCustom;
    ///本地业务标识 。没有用
    TUstpFtdcBusinessLocalIDType	BusinessLocalID;
    ///业务发生日期 。交易日
    TUstpFtdcDateType	ActionDay;



#ifdef OPT_LOG_DEBUG

	int64_t systime = 0;
	get_system_time(&systime);
	EalBson bson;
	bson.AppendInt64("time", systime);

	bson.AppendSymbol("BrokerID", ord.BrokerID);
	bson.AppendSymbol("ExchangeID", ord.ExchangeID);
	bson.AppendSymbol("OrderSysID", ord.OrderSysID);
	bson.AppendSymbol("InvestorID", ord.InvestorID);
	bson.AppendSymbol("UserID", ord.UserID);
	bson.AppendSymbol("InstrumentID", ord.InstrumentID);
	bson.AppendSymbol("UserOrderLocalID", ord.UserOrderLocalID);
	bson.AppendFlag("OrderPriceType", ord.OrderPriceType);
	bson.AppendFlag("Direction", ord.Direction);
	bson.AppendFlag("OffsetFlag", ord.OffsetFlag);
	bson.AppendFlag("HedgeFlag", ord.HedgeFlag);
	bson.AppendDouble("LimitPrice",ord.LimitPrice);
	bson.AppendInt64("Volume",ord.Volume);
	bson.AppendFlag("TimeCondition", ord.TimeCondition);
	bson.AppendSymbol("GTDDate", ord.GTDDate);
	bson.AppendFlag("VolumeCondition", ord.VolumeCondition);
	bson.AppendInt64("MinVolume",ord.MinVolume);
	bson.AppendDouble("StopPrice",ord.StopPrice);
	bson.AppendFlag("ForceCloseReason", ord.ForceCloseReason);
	bson.AppendInt64("MinVolume",ord.MinVolume);
	bson.AppendInt64("IsAutoSuspend",ord.IsAutoSuspend);
	bson.AppendSymbol("BusinessUnit", ord.BusinessUnit);
	bson.AppendSymbol("UserCustom", ord.UserCustom);
	bson.AppendInt64("BusinessLocalID",ord.BusinessLocalID);
	bson.AppendSymbol("ActionDay", ord.ActionDay);


	const char *strJson = bson.GetJsonData();

	logging("[future opt] send ( order ), content: %s", strJson);

	bson.FreeJsonData();

#endif

	//logging_bson_order( (void *)&ord );

    return 0;

}


