#include "stdafx.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"
#include "Emu/IdManager.h"
#include "Emu/SysCalls/SysCalls.h"

#include "Emu/Cell/PPUThread.h"
#include "sleep_queue.h"
#include "sys_time.h"
#include "sys_rwlock.h"

SysCallBase sys_rwlock("sys_rwlock");

s32 sys_rwlock_create(vm::ptr<u32> rw_lock_id, vm::ptr<sys_rwlock_attribute_t> attr)
{
	sys_rwlock.Warning("sys_rwlock_create(rw_lock_id=*0x%x, attr=*0x%x)", rw_lock_id, attr);

	if (!rw_lock_id || !attr)
	{
		return CELL_EFAULT;
	}

	const u32 protocol = attr->protocol;

	switch (protocol)
	{
	case SYS_SYNC_FIFO: break;
	case SYS_SYNC_PRIORITY: break;
	case SYS_SYNC_PRIORITY_INHERIT: break;
	default: sys_rwlock.Error("sys_rwlock_create(): unknown protocol (0x%x)", protocol); return CELL_EINVAL;
	}

	if (attr->pshared.data() != se32(0x200) || attr->ipc_key.data() || attr->flags.data())
	{
		sys_rwlock.Error("sys_rwlock_create(): unknown attributes (pshared=0x%x, ipc_key=0x%llx, flags=0x%x)", attr->pshared, attr->ipc_key, attr->flags);
		return CELL_EINVAL;
	}

	std::shared_ptr<rwlock_t> rwlock(new rwlock_t(attr->protocol, attr->name_u64));

	*rw_lock_id = Emu.GetIdManager().GetNewID(rwlock, TYPE_RWLOCK);

	return CELL_OK;
}

s32 sys_rwlock_destroy(u32 rw_lock_id)
{
	sys_rwlock.Warning("sys_rwlock_destroy(rw_lock_id=%d)", rw_lock_id);

	LV2_LOCK;

	std::shared_ptr<rwlock_t> rwlock;
	if (!Emu.GetIdManager().GetIDData(rw_lock_id, rwlock))
	{
		return CELL_ESRCH;
	}

	if (rwlock.use_count() > 2 || rwlock->readers || rwlock->writer || rwlock->waiters)
	{
		return CELL_EBUSY;
	}

	Emu.GetIdManager().RemoveID(rw_lock_id);

	return CELL_OK;
}

s32 sys_rwlock_rlock(u32 rw_lock_id, u64 timeout)
{
	sys_rwlock.Log("sys_rwlock_rlock(rw_lock_id=%d, timeout=0x%llx)", rw_lock_id, timeout);

	const u64 start_time = get_system_time();

	LV2_LOCK;

	std::shared_ptr<rwlock_t> rwlock;
	if (!Emu.GetIdManager().GetIDData(rw_lock_id, rwlock))
	{
		return CELL_ESRCH;
	}

	while (rwlock->writer || rwlock->waiters)
	{
		if (timeout && get_system_time() - start_time > timeout)
		{
			return CELL_ETIMEDOUT;
		}

		if (Emu.IsStopped())
		{
			sys_rwlock.Warning("sys_rwlock_rlock(id=%d) aborted", rw_lock_id);
			return CELL_OK;
		}

		rwlock->cv.wait_for(lv2_lock, std::chrono::milliseconds(1));
	}

	rwlock->readers++;

	return CELL_OK;
}

s32 sys_rwlock_tryrlock(u32 rw_lock_id)
{
	sys_rwlock.Log("sys_rwlock_tryrlock(rw_lock_id=%d)", rw_lock_id);

	LV2_LOCK;

	std::shared_ptr<rwlock_t> rwlock;
	if (!Emu.GetIdManager().GetIDData(rw_lock_id, rwlock))
	{
		return CELL_ESRCH;
	}

	if (rwlock->writer || rwlock->waiters)
	{
		return CELL_EBUSY;
	}

	rwlock->readers++;

	return CELL_OK;
}

s32 sys_rwlock_runlock(u32 rw_lock_id)
{
	sys_rwlock.Log("sys_rwlock_runlock(rw_lock_id=%d)", rw_lock_id);

	LV2_LOCK;

	std::shared_ptr<rwlock_t> rwlock;
	if (!Emu.GetIdManager().GetIDData(rw_lock_id, rwlock))
	{
		return CELL_ESRCH;
	}

	if (!rwlock->readers)
	{
		return CELL_EPERM;
	}

	if (!--rwlock->readers)
	{
		rwlock->cv.notify_one();
	}

	return CELL_OK;
}

s32 sys_rwlock_wlock(PPUThread& CPU, u32 rw_lock_id, u64 timeout)
{
	sys_rwlock.Log("sys_rwlock_wlock(rw_lock_id=%d, timeout=0x%llx)", rw_lock_id, timeout);

	const u64 start_time = get_system_time();

	LV2_LOCK;

	std::shared_ptr<rwlock_t> rwlock;
	if (!Emu.GetIdManager().GetIDData(rw_lock_id, rwlock))
	{
		return CELL_ESRCH;
	}

	if (rwlock->writer == CPU.GetId())
	{
		return CELL_EDEADLK;
	}

	// protocol is ignored in current implementation
	rwlock->waiters++; assert(rwlock->waiters > 0);

	while (rwlock->readers || rwlock->writer)
	{
		if (timeout && get_system_time() - start_time > timeout)
		{
			rwlock->waiters--; assert(rwlock->waiters >= 0);
			return CELL_ETIMEDOUT;
		}

		if (Emu.IsStopped())
		{
			sys_rwlock.Warning("sys_rwlock_wlock(id=%d) aborted", rw_lock_id);
			return CELL_OK;
		}

		rwlock->cv.wait_for(lv2_lock, std::chrono::milliseconds(1));
	}

	rwlock->writer = CPU.GetId();
	rwlock->waiters--; assert(rwlock->waiters >= 0);

	return CELL_OK;
}

s32 sys_rwlock_trywlock(PPUThread& CPU, u32 rw_lock_id)
{
	sys_rwlock.Log("sys_rwlock_trywlock(rw_lock_id=%d)", rw_lock_id);

	LV2_LOCK;

	std::shared_ptr<rwlock_t> rwlock;
	if (!Emu.GetIdManager().GetIDData(rw_lock_id, rwlock))
	{
		return CELL_ESRCH;
	}

	if (rwlock->writer == CPU.GetId())
	{
		return CELL_EDEADLK;
	}

	if (rwlock->readers || rwlock->writer || rwlock->waiters)
	{
		return CELL_EBUSY;
	}

	rwlock->writer = CPU.GetId();

	return CELL_OK;
}

s32 sys_rwlock_wunlock(PPUThread& CPU, u32 rw_lock_id)
{
	sys_rwlock.Log("sys_rwlock_wunlock(rw_lock_id=%d)", rw_lock_id);

	LV2_LOCK;

	std::shared_ptr<rwlock_t> rwlock;
	if (!Emu.GetIdManager().GetIDData(rw_lock_id, rwlock))
	{
		return CELL_ESRCH;
	}

	if (rwlock->writer != CPU.GetId())
	{
		return CELL_EPERM;
	}

	rwlock->writer = 0;
	rwlock->cv.notify_all();

	return CELL_OK;
}
