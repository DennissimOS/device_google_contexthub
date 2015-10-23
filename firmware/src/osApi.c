#include <syscall.h>
#include <sensors.h>
#include <errno.h>
#include <osApi.h>
#include <util.h>
#include <seos.h>
#include <slab.h>
#include <i2c.h>


static struct SlabAllocator *mSlabAllocator;


static void osExpApiEvtqSubscribe(uintptr_t *retValP, va_list args)
{
    uint32_t tid = va_arg(args, uint32_t);
    uint32_t evtType = va_arg(args, uint32_t);

    *retValP = osEventSubscribe(tid, evtType);
}

static void osExpApiEvtqUnsubscribe(uintptr_t *retValP, va_list args)
{
    uint32_t tid = va_arg(args, uint32_t);
    uint32_t evtType = va_arg(args, uint32_t);

    *retValP = osEventUnsubscribe(tid, evtType);
}

static void osExpApiEvtqEnqueue(uintptr_t *retValP, va_list args)
{
    uint32_t evtType = va_arg(args, uint32_t);
    void *evtData = va_arg(args, void*);
    uint32_t tid = va_arg(args, uint32_t);
    bool external = va_arg(args, int);

    *retValP = osEnqueueEvtAsApp(evtType, evtData, tid, external);
}

static void osExpApiLogLogv(uintptr_t *retValP, va_list args)
{
    enum LogLevel level = va_arg(args, int /* enums promoted to ints in va_args in C */);
    const char *str = va_arg(args, const char*);
    va_list innerArgs = INTEGER_TO_VA_LIST(va_arg(args, uintptr_t));

    osLogv(level, str, innerArgs);
}

static void osExpApiSensorSignal(uintptr_t *retValP, va_list args)
{
    uint32_t handle = va_arg(args, uint32_t);
    uint32_t intEvtNum = va_arg(args, uint32_t);
    uint32_t value1 = va_arg(args, uint32_t);
    uint32_t value2_lo = va_arg(args, uint32_t);
    uint32_t value2_hi = va_arg(args, uint32_t);
    uint64_t value2 = (((uint64_t)value2_hi) << 32) + value2_lo;

    *retValP = (uintptr_t)sensorSignalInternalEvt(handle, intEvtNum, value1, value2);
}

static void osExpApiSensorReg(uintptr_t *retValP, va_list args)
{
    const struct SensorInfo *si = va_arg(args, const struct SensorInfo*);
    uint32_t tid = va_arg(args, uint32_t);

    *retValP = (uintptr_t)sensorRegisterAsApp(si, tid);
}

static void osExpApiSensorUnreg(uintptr_t *retValP, va_list args)
{
    uint32_t handle = va_arg(args, uint32_t);

    *retValP = (uintptr_t)sensorUnregister(handle);
}

static void osExpApiSensorFind(uintptr_t *retValP, va_list args)
{
    uint32_t sensorType = va_arg(args, uint32_t);
    uint32_t idx = va_arg(args, uint32_t);
    uint32_t *handleP = va_arg(args, uint32_t*);

    *retValP = (uintptr_t)sensorFind(sensorType, idx, handleP);
}

static void osExpApiSensorReq(uintptr_t *retValP, va_list args)
{
    uint32_t clientId = va_arg(args, uint32_t);
    uint32_t sensorHandle = va_arg(args, uint32_t);
    uint32_t rate = va_arg(args, uint32_t);
    uint32_t latency = va_arg(args, uint64_t);

    *retValP = sensorRequest(clientId, sensorHandle, rate, latency);
}

static void osExpApiSensorRateChg(uintptr_t *retValP, va_list args)
{
    uint32_t clientId = va_arg(args, uint32_t);
    uint32_t sensorHandle = va_arg(args, uint32_t);
    uint32_t newRate = va_arg(args, uint32_t);
    uint32_t newLatency = va_arg(args, uint64_t);

    *retValP = sensorRequestRateChange(clientId, sensorHandle, newRate, newLatency);
}

static void osExpApiSensorRel(uintptr_t *retValP, va_list args)
{
    uint32_t clientId = va_arg(args, uint32_t);
    uint32_t sensorHandle = va_arg(args, uint32_t);

    *retValP = sensorRelease(clientId, sensorHandle);
}

static void osExpApiSensorTrigger(uintptr_t *retValP, va_list args)
{
    uint32_t clientId = va_arg(args, uint32_t);
    uint32_t sensorHandle = va_arg(args, uint32_t);

    *retValP = sensorTriggerOndemand(clientId, sensorHandle);
}

static void osExpApiSensorGetRate(uintptr_t *retValP, va_list args)
{
    uint32_t sensorHandle = va_arg(args, uint32_t);

    *retValP = sensorGetCurRate(sensorHandle);
}

static union OsApiSlabItem* osExpApiI2cCbkInfoAlloc(uint32_t tid, void *cookie)
{
    union OsApiSlabItem *thing = slabAllocatorAlloc(mSlabAllocator);

    if (thing) {
        thing->i2cAppCbkInfo.toTid = tid;
        thing->i2cAppCbkInfo.cookie = cookie;
    }

    return thing;
}

static void osExpApiI2cInternalEvtFreeF(void *evt)
{
    slabAllocatorFree(mSlabAllocator, evt);
}

static void osExpApiI2cInternalCbk(void *cookie, size_t tx, size_t rx, int err)
{
    union OsApiSlabItem *thing = (union OsApiSlabItem*)cookie;
    uint32_t tid;

    tid = thing->i2cAppCbkInfo.toTid;
    cookie = thing->i2cAppCbkInfo.cookie;

    //we reuse the same slab element to send the event now
    thing->i2cAppCbkEvt.cookie = cookie;
    thing->i2cAppCbkEvt.tx = tx;
    thing->i2cAppCbkEvt.rx = rx;
    thing->i2cAppCbkEvt.err = err;

    if (!osEnqueuePrivateEvt(EVT_APP_I2C_CBK, &thing->i2cAppCbkEvt, osExpApiI2cInternalEvtFreeF, tid)) {
        osLog(LOG_WARN, "Failed to send I2C evt to app. This might end badly for the app...");
        osExpApiI2cInternalEvtFreeF(thing);
    }
}

static void osExpApiI2cMstReq(uintptr_t *retValP, va_list args)
{
    uint32_t busId = va_arg(args, uint32_t);
    uint32_t speed = va_arg(args, uint32_t);

    *retValP = i2cMasterRequest(busId, speed);
}

static void osExpApiI2cMstRel(uintptr_t *retValP, va_list args)
{
    uint32_t busId = va_arg(args, uint32_t);

    *retValP = i2cMasterRelease(busId);
}

static void osExpApiI2cMstTxRx(uintptr_t *retValP, va_list args)
{
    uint32_t busId = va_arg(args, uint32_t);
    uint32_t addr = va_arg(args, uint32_t);
    const void *txBuf = va_arg(args, const void*);
    size_t txSize = va_arg(args, size_t);
    void *rxBuf = va_arg(args, void*);
    size_t rxSize = va_arg(args, size_t);
    uint32_t tid = va_arg(args, uint32_t);
    void *cookie = va_arg(args, void *);
    union OsApiSlabItem *cbkInfo = osExpApiI2cCbkInfoAlloc(tid, cookie);

    if (!cbkInfo)
        *retValP =  -ENOMEM;

    *retValP = i2cMasterTxRx(busId, addr, txBuf, txSize, rxBuf, rxSize, osExpApiI2cInternalCbk, cbkInfo);

    if (*retValP)
        slabAllocatorFree(mSlabAllocator, cbkInfo);
}

static void osExpApiI2cSlvReq(uintptr_t *retValP, va_list args)
{
    uint32_t busId = va_arg(args, uint32_t);
    uint32_t addr = va_arg(args, uint32_t);

    *retValP = i2cSlaveRequest(busId, addr);
}

static void osExpApiI2cSlvRel(uintptr_t *retValP, va_list args)
{
    uint32_t busId = va_arg(args, uint32_t);

    *retValP = i2cSlaveRelease(busId);
}

static void osExpApiI2cSlvRxEn(uintptr_t *retValP, va_list args)
{
    uint32_t busId = va_arg(args, uint32_t);
    void *rxBuf = va_arg(args, void*);
    size_t rxSize = va_arg(args, size_t);
    uint32_t tid = va_arg(args, uint32_t);
    void *cookie = va_arg(args, void *);
    union OsApiSlabItem *cbkInfo = osExpApiI2cCbkInfoAlloc(tid, cookie);

    if (!cbkInfo)
        *retValP =  -ENOMEM;

    i2cSlaveEnableRx(busId, rxBuf, rxSize, osExpApiI2cInternalCbk, cbkInfo);

    if (*retValP)
        slabAllocatorFree(mSlabAllocator, cbkInfo);
}

static void osExpApiI2cSlvTxPre(uintptr_t *retValP, va_list args)
{
    uint32_t busId = va_arg(args, uint32_t);
    uint8_t byte = va_arg(args, int);
    uint32_t tid = va_arg(args, uint32_t);
    void *cookie = va_arg(args, void *);
    union OsApiSlabItem *cbkInfo = osExpApiI2cCbkInfoAlloc(tid, cookie);

    if (!cbkInfo)
        *retValP =  -ENOMEM;

    *retValP = i2cSlaveTxPreamble(busId, byte, osExpApiI2cInternalCbk, cbkInfo);

    if (*retValP)
        slabAllocatorFree(mSlabAllocator, cbkInfo);
}

static void osExpApiI2cSlvTxPkt(uintptr_t *retValP, va_list args)
{
    uint32_t busId = va_arg(args, uint32_t);
    const void *txBuf = va_arg(args, const void*);
    size_t txSize = va_arg(args, size_t);
    uint32_t tid = va_arg(args, uint32_t);
    void *cookie = va_arg(args, void *);
    union OsApiSlabItem *cbkInfo = osExpApiI2cCbkInfoAlloc(tid, cookie);

    if (!cbkInfo)
        *retValP =  -ENOMEM;

    *retValP = i2cSlaveTxPacket(busId, txBuf, txSize, osExpApiI2cInternalCbk, cbkInfo);

    if (*retValP)
        slabAllocatorFree(mSlabAllocator, cbkInfo);
}

void osApiExport(struct SlabAllocator *mainSlubAllocator)
{
    static const struct SyscallTable osMainEvtqTable = {
        .numEntries = SYSCALL_OS_MAIN_EVTQ_LAST,
        .entry = {
            [SYSCALL_OS_MAIN_EVTQ_SUBCRIBE]   = { .func = osExpApiEvtqSubscribe,   },
            [SYSCALL_OS_MAIN_EVTQ_UNSUBCRIBE] = { .func = osExpApiEvtqUnsubscribe, },
            [SYSCALL_OS_MAIN_EVTQ_ENQUEUE]    = { .func = osExpApiEvtqEnqueue,     },
        },
    };

    static const struct SyscallTable osMainLogTable = {
        .numEntries = SYSCALL_OS_MAIN_LOG_LAST,
        .entry = {
            [SYSCALL_OS_MAIN_LOG_LOGV]   = { .func = osExpApiLogLogv,   },
        },
    };

    static const struct SyscallTable osMainSensorsTable = {
        .numEntries = SYSCALL_OS_MAIN_SENSOR_LAST,
        .entry = {
            [SYSCALL_OS_MAIN_SENSOR_SIGNAL]   = { .func = osExpApiSensorSignal,  },
            [SYSCALL_OS_MAIN_SENSOR_REG]      = { .func = osExpApiSensorReg,     },
            [SYSCALL_OS_MAIN_SENSOR_UNREG]    = { .func = osExpApiSensorUnreg,   },
            [SYSCALL_OS_MAIN_SENSOR_FIND]     = { .func = osExpApiSensorFind,    },
            [SYSCALL_OS_MAIN_SENSOR_REQUEST]  = { .func = osExpApiSensorReq,     },
            [SYSCALL_OS_MAIN_SENSOR_RATE_CHG] = { .func = osExpApiSensorRateChg, },
            [SYSCALL_OS_MAIN_SENSOR_RELEASE]  = { .func = osExpApiSensorRel,     },
            [SYSCALL_OS_MAIN_SENSOR_TRIGGER]  = { .func = osExpApiSensorTrigger, },
            [SYSCALL_OS_MAIN_SENSOR_GET_RATE] = { .func = osExpApiSensorGetRate, },

        },
    };

    static const struct SyscallTable osMainTable = {
        .numEntries = SYSCALL_OS_MAIN_LAST,
        .entry = {
            [SYSCALL_OS_MAIN_EVENTQ]  = { .subtable = (struct SyscallTable*)&osMainEvtqTable,    },
            [SYSCALL_OS_MAIN_LOGGING] = { .subtable = (struct SyscallTable*)&osMainLogTable,     },
            [SYSCALL_OS_MAIN_SENSOR]  = { .subtable = (struct SyscallTable*)&osMainSensorsTable, },
        },
    };

    static const struct SyscallTable osDrvGpioTable = {
        .numEntries = SYSCALL_OS_DRV_GPIO_LAST,
        .entry = {
            /* more eventually */
        },
    };

    static const struct SyscallTable osGrvI2cMstTable = {
        .numEntries = SYSCALL_OS_DRV_I2CM_LAST,
        .entry = {
            [SYSCALL_OS_DRV_I2CM_REQ]  = { .func = osExpApiI2cMstReq,  },
            [SYSCALL_OS_DRV_I2CM_REL]  = { .func = osExpApiI2cMstRel,  },
            [SYSCALL_OS_DRV_I2CM_TXRX] = { .func = osExpApiI2cMstTxRx, },
        },
    };

    static const struct SyscallTable osGrvI2cSlvTable = {
        .numEntries = SYSCALL_OS_DRV_I2CS_LAST,
        .entry = {
            [ SYSCALL_OS_DRV_I2CS_REQ]    = { .func = osExpApiI2cSlvReq,   },
            [ SYSCALL_OS_DRV_I2CS_REL]    = { .func = osExpApiI2cSlvRel,   },
            [ SYSCALL_OS_DRV_I2CS_RX_EN]  = { .func = osExpApiI2cSlvRxEn,  },
            [ SYSCALL_OS_DRV_I2CS_TX_PRE] = { .func = osExpApiI2cSlvTxPre, },
            [ SYSCALL_OS_DRV_I2CS_TX_PKT] = { .func = osExpApiI2cSlvTxPkt, },
        },
    };

    static const struct SyscallTable osDriversTable = {
        .numEntries = SYSCALL_OS_DRV_LAST,
        .entry = {
            [SYSCALL_OS_DRV_GPIO]       = { .subtable = (struct SyscallTable*)&osDrvGpioTable,   },
            [SYSCALL_OS_DRV_I2C_MASTER] = { .subtable = (struct SyscallTable*)&osGrvI2cMstTable, },
            [SYSCALL_OS_DRV_I2C_SLAVE]  = { .subtable = (struct SyscallTable*)&osGrvI2cSlvTable, },
        },
    };

    static const struct SyscallTable osTable = {
        .numEntries = SYSCALL_OS_LAST,
        .entry = {
            [SYSCALL_OS_MAIN]    = { .subtable = (struct SyscallTable*)&osMainTable,    },
            [SYSCALL_OS_DRIVERS] = { .subtable = (struct SyscallTable*)&osDriversTable, },
        },
    };

    if (!syscallAddTable(SYSCALL_DOMAIN_OS, 1, (struct SyscallTable*)&osTable))
        osLog(LOG_ERROR, "Failed to export OS base API");
}
