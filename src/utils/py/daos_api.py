#!/usr/bin/python
"""
  (C) Copyright 2018 Intel Corporation.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
  The Government's rights to use, modify, reproduce, release, perform, display,
  or disclose this software are subject to the terms of the Apache License as
  provided in Contract No. B609815.
  Any reproduction of computer software, computer software documentation, or
  portions thereof marked with this legend must also reproduce the markings.
"""
import ctypes
import traceback
import threading
import time
import uuid
import json
import os
import inspect
import sys

from daos_cref import *
from conversion import *

class DaosPool(object):
    """ A python object representing a DAOS pool."""

    def __init__(self, context):
        """ setup the python pool object, not the real pool. """
        self.attached = 0
        self.context = context
        self.uuid = (ctypes.c_ubyte * 1)(0)
        self.group = ctypes.create_string_buffer(b"not set")
        self.handle = ctypes.c_uint64(0)
        self.glob = None
        self.svc = None
        self.pool_info = None
        self.target_info = None

    def get_uuid_str(self):
        return c_uuid_to_str(self.uuid)

    def set_uuid_str(self, uuidstr):
        self.uuid = str_to_c_uuid(uuidstr)

    def create(self, mode, uid, gid, scm_size, group, target_list=None,
               cb_func=None, svcn=1, nvme_size=0):
        """ send a pool creation request to the daos server group """
        c_mode = ctypes.c_uint(mode)
        c_uid = ctypes.c_uint(uid)
        c_gid = ctypes.c_uint(gid)
        c_scm_size = ctypes.c_longlong(scm_size)
        c_nvme_size = ctypes.c_longlong(nvme_size)
        if group is not None:
            self.group = ctypes.create_string_buffer(group)
        else:
            self.group = None
        self.uuid = (ctypes.c_ubyte * 16)()
        rank_t = ctypes.c_uint * svcn
        # initializing with default values
        rank = rank_t(*list([999999 for dummy_i in range(svcn)]))
        rl_ranks = ctypes.POINTER(ctypes.c_uint)(rank)
        c_whatever = ctypes.create_string_buffer(b"rubbish")
        self.svc = RankList(rl_ranks, svcn)

        # assuming for now target list is a server rank list
        if target_list is not None:
            tlist = DaosPool.__pylist_to_array(target_list)
            c_tgts = RankList(tlist, len(tlist))
            tgt_ptr = ctypes.byref(c_tgts)
        else:
            tgt_ptr = None

        func = self.context.get_function('create-pool')

        # the callback function is optional, if not supplied then run the
        # create synchronously, if its there then run it in a thread
        if cb_func == None:
            rc = func(c_mode, c_uid, c_gid, self.group, tgt_ptr,
                      c_whatever, c_scm_size, c_nvme_size,
                      ctypes.byref(self.svc), self.uuid, None)
            if rc != 0:
                self.uuid = (ctypes.c_ubyte * 1)(0)
                raise DaosApiError("Pool create returned non-zero. RC: {0}"
                                 .format(rc))
            else:
                self.attached = 1
        else:
            event = DaosEvent()
            params = [c_mode, c_uid, c_gid, self.group, tgt_ptr,
                      c_whatever, c_scm_size, c_nvme_size,
                      ctypes.byref(self.svc), self.uuid, event]
            t = threading.Thread(target=AsyncWorker1,
                                 args=(func,
                                       params,
                                       self.context,
                                       cb_func,
                                       self))
            t.start()

    def connect(self, flags, cb_func=None):
        """ connect to this pool. """
        # comment this out for now, so we can test bad data
        #if not len(self.uuid) == 16:
        #    raise DaosApiError("No existing UUID for pool.")

        c_flags = ctypes.c_uint(flags)
        c_info = PoolInfo()
        func = self.context.get_function('connect-pool')

        # the callback function is optional, if not supplied then run the
        # create synchronously, if its there then run it in a thread
        if cb_func is None:
            rc = func(self.uuid, self.group, ctypes.byref(self.svc), c_flags,
                      ctypes.byref(self.handle), ctypes.byref(c_info), None)
            if rc != 0:
                self.handle = 0
                raise DaosApiError("Pool connect returned non-zero. RC: {0}"
                                 .format(rc))
        else:
            event = DaosEvent()
            params = [self.uuid, self.group, ctypes.byref(self.svc), c_flags,
                      ctypes.byref(self.handle), ctypes.byref(c_info), event]
            t = threading.Thread(target=AsyncWorker1,
                                 args=(func,
                                       params,
                                       self.context,
                                       cb_func,
                                       self))
            t.start()

    def disconnect(self, cb_func=None):
        """ undoes the fine work done by the connect function above """

        func = self.context.get_function('disconnect-pool')
        if cb_func is None:
            rc = func(self.handle, None)
            if rc != 0:
                raise DaosApiError("Pool disconnect returned non-zero. RC: {0}"
                                 .format(rc))
        else:
            event = DaosEvent()
            params = [self.handle, event]
            t = threading.Thread(target=AsyncWorker1,
                                 args=(func,
                                       params,
                                       self.context,
                                       cb_func,
                                       self))
            t.start()

    def local2global(self):
        """ Create a global pool handle that can be shared. """

        c_glob = IOV()
        c_glob.iov_len = 0
        c_glob.iov_buf_len = 0
        c_glob.iov_buf = None

        func = self.context.get_function("convert-plocal")
        rc = func(self.handle, ctypes.byref(c_glob))
        if rc != 0:
            raise DaosApiError("Pool local2global returned non-zero. RC: {0}"
                             .format(rc))
        # now call it for real
        c_buf = ctypes.create_string_buffer(c_glob.iov_buf_len)
        c_glob.iov_buf = ctypes.cast(c_buf, ctypes.c_void_p)
        rc = func(self.handle, ctypes.byref(c_glob))
        buf = bytearray()
        buf.extend(c_buf.raw)
        return c_glob.iov_len, c_glob.iov_buf_len, buf

    def global2local(self, context, iov_len, buf_len, buf):

        func = self.context.get_function("convert-pglobal")

        c_glob = IOV()
        c_glob.iov_len = iov_len
        c_glob.iov_buf_len = buf_len
        c_buf = ctypes.create_string_buffer(str(buf))
        c_glob.iov_buf = ctypes.cast(c_buf, ctypes.c_void_p)

        local_handle = ctypes.c_uint64(0)
        rc = func(c_glob, ctypes.byref(local_handle))
        if rc != 0:
            raise DaosApiError("Pool global2local returned non-zero. RC: {0}"
                             .format(rc))
        self.handle = local_handle
        return local_handle

    def exclude(self, tgt_rank_list, cb_func=None):
        """Exclude a set of storage targets from a pool."""

        if tgt_rank_list is None:
            c_tgts = None
        else:
            rl_ranks = DaosPool.__pylist_to_array(tgt_rank_list)
            c_tgts = ctypes.pointer(RankList(rl_ranks, len(tgt_rank_list)))

        if self.svc is None:
            c_svc = None
        else:
            c_svc = ctypes.pointer(self.svc)

        func = self.context.get_function('exclude-target')
        if cb_func is None:
            rc = func(self.uuid, self.group, c_svc,
                      c_tgts, None)
            if rc != 0:
                raise DaosApiError("Pool exclude returned non-zero. RC: {0}"
                                 .format(rc))
        else:
            event = DaosEvent()
            params = [self.uuid, self.group, c_svc,
                      ctypes.byref(c_tgts), event]
            t = threading.Thread(target=AsyncWorker1,
                                 args=(func,
                                       params,
                                       self.context,
                                       cb_func,
                                       self))
            t.start()

    def extend(self):
        """Extend the pool to more targets."""

        raise NotImplementedError("Extend not implemented in C API yet.")

    def evict(self, cb_func=None):
        """Evict all connections to a pool."""

        func = self.context.get_function('evict-client')

        if cb_func is None:
            rc = func(self.uuid, self.group, ctypes.byref(self.svc), None)
            if rc != 0:
                raise DaosApiError(
                    "Pool evict returned non-zero. RC: {0}".format(rc))
        else:
            event = DaosEvent()
            params = [self.uuid, self.group, ctypes.byref(self.svc), event]
            t = threading.Thread(target=AsyncWorker1,
                                 args=(func,
                                       params,
                                       self.context,
                                       cb_func,
                                       self))
            t.start()

    def tgt_add(self, tgt_rank_list, cb_func=None):
        """add a set of storage targets to a pool."""

        rl_ranks = DaosPool.__pylist_to_array(tgt_rank_list)
        c_tgts = RankList(rl_ranks, len(tgt_rank_list))
        func = self.context.get_function("add-target")

        if cb_func is None:
            rc = func(self.uuid, self.group, ctypes.byref(self.svc),
                      ctypes.byref(c_tgts), None)
            if rc != 0:
                raise DaosApiError("Pool tgt_add returned non-zero. RC: {0}"
                                 .format(rc))
        else:
            event = DaosEvent()
            params = [self.uuid, self.group, ctypes.byref(self.svc),
                      ctypes.byref(c_tgts), event]
            t = threading.Thread(target=AsyncWorker1,
                                 args=(func,
                                       params,
                                       self.context,
                                       cb_func,
                                       self))
            t.start()

    def exclude_out(self, tgt_rank_list, cb_func=None):
        """Exclude completely a set of storage targets from a pool."""

        rl_ranks = DaosPool.__pylist_to_array(tgt_rank_list)
        c_tgts = RankList(rl_ranks, len(tgt_rank_list))
        func = self.context.get_function('kill-target')

        if cb_func is None:
            rc = func(self.uuid, self.group, ctypes.byref(self.svc),
                      ctypes.byref(c_tgts), None)
            if rc != 0:
                raise DaosApiError("Pool exclude_out returned non-zero. RC: {0}"
                                 .format(rc))
        else:
            event = DaosEvent()
            params = [self.uuid, self.group, ctypes.byref(self.svc),
                      ctypes.byref(c_tgts), event]
            t = threading.Thread(target=AsyncWorker1,
                                 args=(func,
                                       params,
                                       self.context,
                                       cb_func,
                                       self))
            t.start()

    def pool_svc_stop(self, cb_func=None):
        """Stop the current pool service leader."""

        func = self.context.get_function('service-stop')

        if cb_func is None:
            rc = func(self.handle, None)
            if rc != 0:
                raise DaosApiError("Pool svc_Stop returned non-zero. RC: {0}"
                                 .format(rc))
        else:
            event = DaosEvent()
            params = [self.handle, event]
            t = threading.Thread(target=AsyncWorker1,
                                 args=(func,
                                       params,
                                       self.context,
                                       cb_func, self))
            t.start()

    def pool_query(self, cb_func=None):
        """Query pool information."""

        self.pool_info = PoolInfo()
        func = self.context.get_function('query-pool')

        if cb_func is None:
            rc = func(self.handle, None, ctypes.byref(self.pool_info), None)
            if rc != 0:
                raise DaosApiError("Pool query returned non-zero. RC: {0}"
                                 .format(rc))
            return self.pool_info
        else:
            event = DaosEvent()
            params = [self.handle, None, ctypes.byref(self.pool_info), event]
            t = threading.Thread(target=AsyncWorker1,
                                 args=(func,
                                       params,
                                       self.context,
                                       cb_func,
                                       self))
            t.start()
        return None

    def target_query(self, tgt):
        """Query information of storage targets within a DAOS pool."""
        raise NotImplementedError("Target_query not yet implemented in C API.")

    def destroy(self, force, cb_func=None):

        if not len(self.uuid) == 16 or self.attached == 0:
            raise DaosApiError("No existing UUID for pool.")

        c_force = ctypes.c_uint(force)
        func = self.context.get_function('destroy-pool')

        if cb_func is None:
            rc = func(self.uuid, self.group, c_force, None)
            if rc != 0:
                raise DaosApiError("Pool destroy returned non-zero. RC: {0}"
                                 .format(rc))
            else:
                self.attached = 0
        else:
            event = DaosEvent()
            params = [self.uuid, self.group, c_force, event]

            t = threading.Thread(target=AsyncWorker1,
                                 args=(func,
                                       params,
                                       self.context,
                                       cb_func, self))
            t.start()

    def set_svc(self, rank):
         """
         note support for a single rank only
         """
         svc_rank = ctypes.c_uint(rank)
         rl_ranks = ctypes.POINTER(ctypes.c_uint)(svc_rank)
         self.svc = RankList(rl_ranks, 1)

    @staticmethod
    def __pylist_to_array(pylist):

        return (ctypes.c_uint32 * len(pylist))(*pylist)

class DaosObj(object):
    """ A class representing an object stored in a DAOS container.  """

    def __init__(self, context, container, c_oid=None):
        self.context = context
        self.container = container
        self.c_oid = c_oid
        self.c_tgts = None
        self.attr = None
        self.oh = None
        self.tgt_rank_list = []

    def __del__(self):
        """ clean up this object """
        if self.oh is not None:
            func = self.context.get_function('close-obj')
            rc = func(self.oh, None)
            if rc != 0:
                raise DaosApiError("Object close returned non-zero. RC: {0}"
                                 .format(rc))
            self.oh = None

    def create(self, rank=None, objcls=13):
        """ generate a random oid """
        func = self.context.get_function('generate-oid')

        func.restype = DaosObjId
        self.c_oid = func(objcls, 0, 0)
        if rank is not None:
            self.c_oid.hi |= rank << 24

    def open(self, epoch=0):
        """ open the object so we can interact with it """
        func = self.context.get_function('open-obj')

        c_epoch = ctypes.c_uint64(epoch)
        c_mode = ctypes.c_uint(4)
        self.oh = ctypes.c_uint64(0)

        rc = func(self.container.coh, self.c_oid, c_epoch, c_mode,
                  ctypes.byref(self.oh), None)
        if rc != 0:
            raise DaosApiError("Object open returned non-zero. RC: {0}"
                             .format(rc))

    def close(self):
        """ close this object """
        if self.oh is not None:
            func = self.context.get_function('close-obj')
            rc = func(self.oh, None)
            if rc != 0:
                raise DaosApiError("Object close returned non-zero. RC: {0}"
                                 .format(rc))
            self.oh = None

    def refresh_attr(self, epoch):
        """ Get object attributes and save internally

            NOTE: THIS FUNCTION ISN'T IMPLEMENTED ON THE DAOS SIDE
        """

        if self.c_oid is None:
            raise DaosApiError("refresh_attr called but object not initialized")
        if self.oh is None:
            self.open()

        c_epoch = ctypes.c_uint64(epoch)
        rank_list = ctypes.cast(ctypes.pointer((ctypes.c_uint32 * 5)()),
                                ctypes.POINTER(ctypes.c_uint32))
        self.c_tgts = RankList(rank_list, 5)

        func = self.context.get_function('query-obj')
        rc = func(self.oh, c_epoch, None, self.c_tgts, None)

    def get_layout(self):
        """ Get object target layout info

            NOTE: THIS FUNCTION ISN'T PART OF THE PUBLIC API
        """
        if self.c_oid is None:
            raise DaosApiError("get_layout called but object is not initialized")
        if self.oh is None:
            self.open()

        obj_layout_ptr = ctypes.POINTER(DaosObjLayout)()

        func = self.context.get_function('get-layout')
        rc = func(self.container.coh, self.c_oid, ctypes.byref(obj_layout_ptr))

        if rc == 0:
            shards = obj_layout_ptr[0].ol_shards[0][0].os_replica_nr
            del self.tgt_rank_list[:]
            for i in range(0, shards):
                self.tgt_rank_list.append(
                    obj_layout_ptr[0].ol_shards[0][0].os_ranks[i])
        else:
            raise DaosApiError("get_layout returned non-zero. RC: {0}".format(rc))


    def punch(self, epoch, cb_func=None):
        """ Delete this object but only from the specified epoch

        Function arguments:
        epoch    --the epoch from which keys will be deleted.
        cb_func  --an optional callback function
        """

        if self.oh is None:
            self.open()

        c_epoch = ctypes.c_uint64(epoch)

        # the callback function is optional, if not supplied then run the
        # punch synchronously, if its there then run it in a thread
        func = self.context.get_function('punch-obj')
        if cb_func == None:
            rc = func(self.oh, c_epoch, None)
            if rc != 0:
                raise DaosApiError("punch-dkeys returned non-zero. RC: {0}"
                                 .format(rc))
        else:
            event = DaosEvent()
            params = [self.oh, c_epoch, event]
            t = threading.Thread(target=AsyncWorker1,
                                 args=(func,
                                       params,
                                       self.context,
                                       cb_func,
                                       self))
            t.start()


    def punch_dkeys(self, epoch, dkeys, cb_func=None):
        """ Deletes dkeys and associated data from an object for a specific
        epoch.

        Function arguments:
        epoch    --the epoch from which keys will be deleted.
        dkeys    --the keys to be deleted, None will be passed as NULL
        cb_func  --an optional callback function
        """
        if self.oh is None:
            self.open()

        c_epoch = ctypes.c_uint64(epoch)

        if dkeys is None:
            c_len_dkeys = 0
            c_dkeys = None
        else:
            c_len_dkeys = ctypes.c_uint(len(dkeys))
            c_dkeys = (IOV * len(dkeys))()
            i = 0
            for dkey in dkeys:
                c_dkey = ctypes.create_string_buffer(dkey)
                c_dkeys[i].iov_buf = ctypes.cast(c_dkey, ctypes.c_void_p)
                c_dkeys[i].iov_buf_len = ctypes.sizeof(c_dkey)
                c_dkeys[i].iov_len = ctypes.sizeof(c_dkey)
                i += 1

        # the callback function is optional, if not supplied then run the
        # create synchronously, if its there then run it in a thread
        func = self.context.get_function('punch-dkeys')
        if cb_func == None:
            rc = func(self.oh, c_epoch, c_len_dkeys, ctypes.byref(c_dkeys),
                      None)
            if rc != 0:
                raise DaosApiError("punch-dkeys returned non-zero. RC: {0}"
                                 .format(rc))
        else:
            event = DaosEvent()
            params = [self.oh, c_epoch, c_len_dkeys, ctypes.byref(c_dkeys),
                      event]
            t = threading.Thread(target=AsyncWorker1,
                                 args=(func,
                                       params,
                                       self.context,
                                       cb_func,
                                       self))
            t.start()

    def punch_akeys(self, epoch, dkey, akeys, cb_func=None):
        """ Deletes akeys and associated data from a dkey/object for a specific
        epoch.

        Function arguments:
        epoch    --the epoch from which keys will be deleted.
        dkey     --the parent dkey from which the akeys will be deleted,
                   Expecting a string
        akeys    --a list of akeys (strings) which are to be deleted
        cb_func  --an optional callback function
        """
        if self.oh is None:
            self.open()

        c_epoch = ctypes.c_uint64(epoch)

        c_dkey_iov = IOV()
        c_dkey = ctypes.create_string_buffer(dkey)
        c_dkey_iov.iov_buf = ctypes.cast(c_dkey, ctypes.c_void_p)
        c_dkey_iov.iov_buf_len = ctypes.sizeof(c_dkey)
        c_dkey_iov.iov_len = ctypes.sizeof(c_dkey)

        c_len_akeys = ctypes.c_uint(len(akeys))
        c_akeys = (IOV * len(akeys))()
        i = 0
        for akey in akeys:
            c_akey = ctypes.create_string_buffer(akey)
            c_akeys[i].iov_buf = ctypes.cast(c_akey, ctypes.c_void_p)
            c_akeys[i].iov_buf_len = ctypes.sizeof(c_akey)
            c_akeys[i].iov_len = ctypes.sizeof(c_akey)
            i += 1

        # the callback function is optional, if not supplied then run the
        # create synchronously, if its there then run it in a thread
        func = self.context.get_function('punch-akeys')
        if cb_func == None:
            rc = func(self.oh, c_epoch, ctypes.byref(c_dkey_iov), c_len_akeys,
                      ctypes.byref(c_akeys), None)
            if rc != 0:
                raise DaosApiError("punch-akeys returned non-zero. RC: {0}"
                                 .format(rc))
        else:
            event = DaosEvent()
            params = [self.oh, c_epoch, ctypes.byref(c_dkey_iov), c_len_akeys,
                      ctypes.byref(c_akeys), event]
            t = threading.Thread(target=AsyncWorker1,
                                 args=(func,
                                       params,
                                       self.context,
                                       cb_func,
                                       self))
            t.start()

class IORequest(object):
    """
    Python object that centralizes details about an I/O
    type is either 1 (single) or 2 (array)
    """
    def __init__(self, context, container, obj, rank=None, iotype=1,
                 objtype=13):
        """
        container --which container the object is (or will be) in
        obj --None to create a new object or the OID of an existing obj
        rank --utilize with certain object types to force obj to a specific
               server
        iotype --1 for single, 2 for array
        objtype --specifies the attributes for the object
        """
        self.context = context
        self.container = container

        if obj is None:
            # create a new object
            self.obj = DaosObj(context, container)
            self.obj.create(rank, objtype)
            self.obj.open()
        else:
            self.obj = obj

        self.io_type = ctypes.c_int(iotype)

        self.sgl = SGL()

        self.iod = DaosIODescriptor()
        ctypes.memset(ctypes.byref(self.iod.iod_kcsum), 0, 16)

        self.epoch_range = EpochRange()

        cs = CheckSum()
        cs.cs_sum = ctypes.pointer(ctypes.create_string_buffer(32))
        cs.cs_buf_len = 32
        cs.cs_len = 0
        self.iod.iod_csums = ctypes.pointer(cs)

    def __del__(self):
        """ cleanup this request """
        pass

    def insert_array(self, dkey, akey, c_data, epoch):
        """
        Setup the I/O Vector and I/O descriptor for an array insertion.
        This function is limited to a single descriptor and a single
        scatter gather list.  The single SGL can have any number of
        entries as dictated by the c_data parameter.
        """

        sgl_iov_list = (IOV * len(c_data))()
        idx = 0
        for item in c_data:
             sgl_iov_list[idx].iov_len = item[1]
             sgl_iov_list[idx].iov_buf_len = item[1]
             sgl_iov_list[idx].iov_buf = ctypes.cast(item[0], ctypes.c_void_p)
             idx += 1

        self.sgl.sg_iovs = ctypes.cast(ctypes.pointer(sgl_iov_list),
                                       ctypes.POINTER(IOV))
        self.sgl.sg_nr = len(c_data)
        self.sgl.sg_nr_out = len(c_data)

        self.epoch_range.epr_lo = epoch
        self.epoch_range.epr_hi = ~0

        extent = Extent()
        extent.rx_idx = 0
        extent.rx_nr = len(c_data)

        # setup the descriptor
        self.iod.iod_name.iov_buf = ctypes.cast(akey, ctypes.c_void_p)
        self.iod.iod_name.iov_buf_len = ctypes.sizeof(akey)
        self.iod.iod_name.iov_len = ctypes.sizeof(akey)
        self.iod.iod_type = 2
        self.iod.iod_size = c_data[0][1]
        self.iod.iod_nr = 1
        self.iod.iod_recxs = ctypes.pointer(extent)
        self.iod.iod_eprs = ctypes.cast(ctypes.pointer(self.epoch_range),
                                        ctypes.c_void_p)

        # now do it
        func = self.context.get_function('update-obj')

        dkey_iov = IOV()
        dkey_iov.iov_buf = ctypes.cast(dkey, ctypes.c_void_p)
        dkey_iov.iov_buf_len = ctypes.sizeof(dkey)
        dkey_iov.iov_len = ctypes.sizeof(dkey)

        rc = func(self.obj.oh, self.epoch_range.epr_lo, ctypes.byref(dkey_iov),
                  1, ctypes.byref(self.iod), ctypes.byref(self.sgl), None)
        if rc != 0:
            raise DaosApiError("Object update returned non-zero. RC: {0}"
                             .format(rc))

    def fetch_array(self, dkey, akey, rec_count, rec_size, epoch):
        """
        dkey --1st level key for the array value
        akey --2nd level key for the array value
        rec_count --how many array indices (records) to retrieve
        rec_size --size in bytes of a single record
        epoch --which epoch to read the value from
        """

        # setup the descriptor, we are only handling a single descriptor that
        # covers an arbitrary number of consecutive array entries
        extent = Extent()
        extent.rx_idx = 0
        extent.rx_nr = ctypes.c_ulong(rec_count.value)

        self.iod.iod_name.iov_buf = ctypes.cast(akey, ctypes.c_void_p)
        self.iod.iod_name.iov_buf_len = ctypes.sizeof(akey)
        self.iod.iod_name.iov_len = ctypes.sizeof(akey)
        self.iod.iod_type = 2
        self.iod.iod_size = rec_size
        self.iod.iod_nr = 1
        self.iod.iod_recxs = ctypes.pointer(extent)

        # setup the scatter/gather list, we are only handling an
        # an arbitrary number of consecutive array entries of the same size
        sgl_iov_list = (IOV * rec_count.value)()
        for i in range(rec_count.value):
             sgl_iov_list[i].iov_buf_len = rec_size
             sgl_iov_list[i].iov_buf = ctypes.cast(
                 ctypes.create_string_buffer(rec_size.value),
                 ctypes.c_void_p)
        self.sgl.sg_iovs = ctypes.cast(ctypes.pointer(sgl_iov_list),
                                       ctypes.POINTER(IOV))
        self.sgl.sg_nr = rec_count
        self.sgl.sg_nr_out = rec_count

        dkey_iov = IOV()
        dkey_iov.iov_buf = ctypes.cast(dkey, ctypes.c_void_p)
        dkey_iov.iov_buf_len = ctypes.sizeof(dkey)
        dkey_iov.iov_len = ctypes.sizeof(dkey)

        # now do it
        func = self.context.get_function('fetch-obj')

        rc = func(self.obj.oh, epoch, ctypes.byref(dkey_iov), 1,
                  ctypes.byref(self.iod), ctypes.byref(self.sgl), None, None)
        if rc != 0:
            raise DaosApiError("Array fetch returned non-zero. RC: {0}"
                             .format(rc))

        # convert the output into a python list rather than return C types
        # outside this file
        output = []
        for i in range(rec_count.value):
            output.append(ctypes.string_at(sgl_iov_list[i].iov_buf,
                                           rec_size.value))
        return output

    def single_insert(self, dkey, akey, value, size, epoch):
        """
        dkey --1st level key for the array value
        akey --2nd level key for the array value
        value --string value to insert
        size --size of the string
        epoch --which epoch to write to
        """

        # put the data into the scatter gather list
        sgl_iov = IOV()
        sgl_iov.iov_len = size
        sgl_iov.iov_buf_len = size
        if value is not None:
            sgl_iov.iov_buf = ctypes.cast(value, ctypes.c_void_p)
        # testing only path
        else:
            sgl_iov.iov_buf = None
        self.sgl.sg_iovs = ctypes.pointer(sgl_iov)
        self.sgl.sg_nr = 1
        self.sgl.sg_nr_out = 1

        self.epoch_range.epr_lo = epoch
        self.epoch_range.epr_hi = ~0

        # setup the descriptor
        if akey is not None:
            self.iod.iod_name.iov_buf = ctypes.cast(akey, ctypes.c_void_p)
            self.iod.iod_name.iov_buf_len = ctypes.sizeof(akey)
            self.iod.iod_name.iov_len = ctypes.sizeof(akey)
            self.iod.iod_type = 1
            self.iod.iod_size = size
            self.iod.iod_nr = 1
            self.iod.iod_eprs = ctypes.cast(ctypes.pointer(self.epoch_range),
                                            ctypes.c_void_p)
            iod_ptr = ctypes.pointer(self.iod)
        else:
            iod_ptr = None

        # now do it
        func = self.context.get_function('update-obj')

        if dkey is not None:
            dkey_iov = IOV()
            dkey_iov.iov_buf = ctypes.cast(dkey, ctypes.c_void_p)
            dkey_iov.iov_buf_len = ctypes.sizeof(dkey)
            dkey_iov.iov_len = ctypes.sizeof(dkey)
            dkey_ptr = ctypes.pointer(dkey_iov)
        else:
            dkey_ptr = None

        rc = func(self.obj.oh, self.epoch_range.epr_lo, dkey_ptr, 1,
                  ctypes.byref(self.iod), ctypes.byref(self.sgl), None)
        if rc != 0:
            raise DaosApiError("Object update returned non-zero. RC: {0}"
                             .format(rc))

    def single_fetch(self, dkey, akey, size, epoch, test_hints=[]):
        """
        dkey --1st level key for the single value
        akey --2nd level key for the single value
        size --size of the string
        epoch --which epoch to read from
        test_hints --optional set of values that allow for error injection,
            supported values 'sglnull', 'iodnull'.

        a string containing the value is returned
        """
        if any("sglnull" in s for s in test_hints):
            sgl_ptr = None
            buf = ctypes.create_string_buffer(0)
        else:
            sgl_iov = IOV()
            sgl_iov.iov_len = ctypes.c_size_t(size)
            sgl_iov.iov_buf_len = ctypes.c_size_t(size)

            buf = ctypes.create_string_buffer(size)
            sgl_iov.iov_buf = ctypes.cast(buf, ctypes.c_void_p)
            self.sgl.sg_iovs = ctypes.pointer(sgl_iov)
            self.sgl.sg_nr = 1
            self.sgl.sg_nr_out = 1

            sgl_ptr = ctypes.pointer(self.sgl)

        self.epoch_range.epr_lo = epoch
        self.epoch_range.epr_hi = ~0

        # setup the descriptor

        if any("iodnull" in s for s in test_hints):
            iod_ptr = None
        else:
            self.iod.iod_name.iov_buf = ctypes.cast(akey, ctypes.c_void_p)
            self.iod.iod_name.iov_buf_len = ctypes.sizeof(akey)
            self.iod.iod_name.iov_len = ctypes.sizeof(akey)
            self.iod.iod_type = 1
            self.iod.iod_size = ctypes.c_size_t(size)
            self.iod.iod_nr = 1
            self.iod.iod_eprs = ctypes.cast(ctypes.pointer(self.epoch_range),
                                            ctypes.c_void_p)
            iod_ptr = ctypes.pointer(self.iod)

        if dkey is not None:
            dkey_iov = IOV()
            dkey_iov.iov_buf = ctypes.cast(dkey, ctypes.c_void_p)
            dkey_iov.iov_buf_len = ctypes.sizeof(dkey)
            dkey_iov.iov_len = ctypes.sizeof(dkey)
            dkey_ptr = ctypes.pointer(dkey_iov)
        else:
            dkey_ptr = None

        # now do it
        func = self.context.get_function('fetch-obj')
        rc = func(self.obj.oh, self.epoch_range.epr_lo, dkey_ptr,
                  1, iod_ptr, sgl_ptr, None, None)
        if rc != 0:
            raise DaosApiError("Object fetch returned non-zero. RC: {0}"
                             .format(rc))
        return buf

    def multi_akey_insert(self, dkey, data, epoch):
        """
        Update object with with multiple values, where each value is tagged
        with an akey.  This is a bit of a mess but need to refactor all the
        I/O functions as a group at some point.

        dkey  --1st level key for the values
        data  --a list of tuples (akey, value)
        epoch --which epoch to write to
        """

        # put the data into the scatter gather list
        count = len(data)
        c_count = ctypes.c_uint(count)
        iods = (DaosIODescriptor * count)()
        sgl_list = (SGL * count)()
        i=0
        for tup in data:

            sgl_iov = IOV()
            sgl_iov.iov_len = ctypes.c_size_t(len(tup[1])+1)
            sgl_iov.iov_buf_len = ctypes.c_size_t(len(tup[1])+1)
            sgl_iov.iov_buf = ctypes.cast(tup[1], ctypes.c_void_p)

            sgl_list[i].sg_nr_out = 1
            sgl_list[i].sg_nr = 1
            sgl_list[i].sg_iovs = ctypes.pointer(sgl_iov)

            iods[i].iod_name.iov_buf = ctypes.cast(tup[0], ctypes.c_void_p)
            iods[i].iod_name.iov_buf_len = ctypes.sizeof(tup[0])
            iods[i].iod_name.iov_len = ctypes.sizeof(tup[0])
            iods[i].iod_type = 1
            iods[i].iod_size = len(tup[1])+1
            iods[i].iod_nr = 1
            ctypes.memset(ctypes.byref(iods[i].iod_kcsum), 0, 16)
            i += 1
        iod_ptr = ctypes.pointer(iods)
        sgl_ptr = ctypes.pointer(sgl_list)

        if dkey is not None:
            dkey_iov = IOV()
            dkey_iov.iov_buf = ctypes.cast(dkey, ctypes.c_void_p)
            dkey_iov.iov_buf_len = ctypes.sizeof(dkey)
            dkey_iov.iov_len = ctypes.sizeof(dkey)
            dkey_ptr = ctypes.pointer(dkey_iov)
        else:
            dkey_ptr = None

        # now do it
        func = self.context.get_function('update-obj')
        rc = func(self.obj.oh, epoch, dkey_ptr, c_count,
                  iod_ptr, sgl_ptr, None)
        if rc != 0:
            raise DaosApiError("Object update returned non-zero. RC: {0}"
                             .format(rc))

    def multi_akey_fetch(self, dkey, keys, epoch):
        """
        Retrieve multiple akeys & associated data.  This is kind of a mess but
        will refactor all the I/O functions at some point.

        dkey --1st level key for the array value
        keys --a list of tuples where each tuple is an (akey, size), where size
             is the size of the data for that key
        epoch --which epoch to read from

        returns a dictionary containing the akey:value pairs
        """

        # create scatter gather list to hold the returned data also
        # create the descriptor
        count = len(keys)
        c_count = ctypes.c_uint(count)
        i = 0
        sgl_list = (SGL * count)()
        iods = (DaosIODescriptor * count)()
        for key in keys:
            sgl_iov = IOV()
            sgl_iov.iov_len = ctypes.c_ulong(key[1].value+1)
            sgl_iov.iov_buf_len = ctypes.c_ulong(key[1].value+1)
            buf = ctypes.create_string_buffer(key[1].value+1)
            sgl_iov.iov_buf = ctypes.cast(buf, ctypes.c_void_p)

            sgl_list[i].sg_nr_out = 1
            sgl_list[i].sg_nr = 1
            sgl_list[i].sg_iovs = ctypes.pointer(sgl_iov)

            iods[i].iod_name.iov_buf = ctypes.cast(key[0], ctypes.c_void_p)
            iods[i].iod_name.iov_buf_len = ctypes.sizeof(key[0])
            iods[i].iod_name.iov_len = ctypes.sizeof(key[0])
            iods[i].iod_type = 1
            iods[i].iod_size = ctypes.c_ulong(key[1].value+1)

            iods[i].iod_nr = 1
            ctypes.memset(ctypes.byref(iods[i].iod_kcsum), 0, 16)
            i += 1
        iod_ptr = ctypes.pointer(iods)
        sgl_ptr = ctypes.pointer(sgl_list)

        dkey_iov = IOV()
        dkey_iov.iov_buf = ctypes.cast(dkey, ctypes.c_void_p)
        dkey_iov.iov_buf_len = ctypes.sizeof(dkey)
        dkey_iov.iov_len = ctypes.sizeof(dkey)

        # now do it
        func = self.context.get_function('fetch-obj')

        rc = func(self.obj.oh, epoch, ctypes.byref(dkey_iov),
                  c_count, ctypes.byref(iods), sgl_ptr, None, None)
        if rc != 0:
            raise DaosApiError("multikey fetch returned non-zero. RC: {0}"
                             .format(rc))

        result = {}
        i = 0
        for sgl in sgl_list:
            p = ctypes.cast((sgl.sg_iovs).contents.iov_buf, ctypes.c_char_p)
            result[(keys[i][0]).value] = p.value
            i += 1

        return result

class DaosContainer(object):
    """ A python object representing a DAOS container."""

    def __init__(self, context, cuuid=None, poh=None, coh=None):
        """ setup the python container object, not the real container. """
        self.context = context

        # ignoring caller parameters for now

        self.uuid = (ctypes.c_ubyte * 1)(0)
        self.coh = ctypes.c_uint64(0)
        self.poh = ctypes.c_uint64(0)
        self.info = ContInfo()

    def get_uuid_str(self):
        return c_uuid_to_str(self.uuid)

    def create(self, poh, con_uuid=None, cb_func=None):
        """ send a container creation request to the daos server group """

        # create a random uuid if none is provided
        self.uuid = (ctypes.c_ubyte * 16)()
        if con_uuid is None:
            c_uuid(uuid.uuid4(), self.uuid)
        else:
            c_uuid(con_uuid, self.uuid)

        self.poh = poh

        func = self.context.get_function('create-cont')

        # the callback function is optional, if not supplied then run the
        # create synchronously, if its there then run it in a thread
        if cb_func == None:
            rc = func(self.poh, self.uuid, None)
            if rc != 0:
                self.uuid = (ctypes.c_ubyte * 1)(0)
                raise DaosApiError("Container create returned non-zero. RC: {0}"
                                 .format(rc))
        else:
            event = DaosEvent()
            params = [self.poh, self.uuid, event]
            t = threading.Thread(target=AsyncWorker1,
                                 args=(func,
                                       params,
                                       self.context,
                                       cb_func,
                                       self))
            t.start()

    def destroy(self, force=1, poh=None, con_uuid=None, cb_func=None):
        """ send a container destroy request to the daos server group """

        # caller can override pool handle and uuid
        if poh is not None:
            self.poh = poh
        if con_uuid is not None:
            c_uuid(con_uuid, self.uuid)

        c_force = ctypes.c_uint(force)

        func = self.context.get_function('destroy-cont')

        # the callback function is optional, if not supplied then run the
        # create synchronously, if its there then run it in a thread
        if cb_func == None:
            rc = func(self.poh, self.uuid, c_force, None)
            if rc != 0:
                raise DaosApiError("Container destroy returned non-zero. RC: {0}"
                                 .format(rc))
        else:
            event = DaosEvent()
            params = [self.poh, self.uuid, c_force, event]
            t = threading.Thread(target=AsyncWorker1,
                                 args=(func,
                                       params,
                                       self.context,
                                       cb_func,
                                       self))
            t.start()

    def open(self, poh=None, cuuid=None, flags=None, cb_func=None):
        """ send a container open request to the daos server group """

        # parameters can be used to associate this python object with a
        # DAOS container or they may already have been set
        if poh is not None:
            self.poh = poh
        if cuuid is not None:
            c_uuid(cuuid, self.uuid)

        # Note that 2 is read/write
        c_flags = ctypes.c_uint(2)
        if flags is not None:
            c_flags = ctypes.c_uint(flags)

        func = self.context.get_function('open-cont')

        # the callback function is optional, if not supplied then run the
        # create synchronously, if its there then run it in a thread
        if cb_func == None:
            rc = func(self.poh, self.uuid, c_flags, ctypes.byref(self.coh),
                      ctypes.byref(self.info), None)
            if rc != 0:
                raise DaosApiError("Container open returned non-zero. RC: {0}"
                                 .format(rc))
        else:
            event = DaosEvent()
            params = [self.poh, self.uuid, c_flags, ctypes.byref(self.coh),
                      ctypes.byref(self.info), event]
            t = threading.Thread(target=AsyncWorker1,
                                 args=(func,
                                       params,
                                       self.context,
                                       cb_func,
                                       self))
            t.start()

    def close(self, coh=None, cb_func=None):
        """ send a container close request to the daos server group """

        # parameters can be used to associate this python object with a
        # DAOS container or they may already have been set
        if coh is not None:
            self.coh = coh

        func = self.context.get_function('close-cont')

        # the callback function is optional, if not supplied then run the
        # create synchronously, if its there then run it in a thread
        if cb_func == None:
            rc = func(self.coh, None)
            if rc != 0:
                raise DaosApiError("Container close returned non-zero. RC: {0}"
                                 .format(rc))
        else:
            event = DaosEvent()
            params = [self.coh, event]
            t = threading.Thread(target=AsyncWorker1,
                                 args=(func,
                                       params,
                                       self.context,
                                       cb_func,
                                       self))
            t.start()

    def query(self, coh=None, cb_func=None):
        """Query container information."""

        # allow caller to override the handle
        if coh is not None:
            self.coh = coh

        func = self.context.get_function('query-cont')

        if cb_func is None:
            rc = func(self.coh, ctypes.byref(self.info), None)
            if rc != 0:
                raise DaosApiError("Container query returned non-zero. RC: {0}"
                                 .format(rc))
            return self.info
        else:
            event = DaosEvent()
            params = [self.coh, ctypes.byref(self.info), event]
            t = threading.Thread(target=AsyncWorker1,
                                 args=(func,
                                       params,
                                       self.context,
                                       cb_func,
                                       self))
            t.start()
        return None

    def get_new_epoch(self):
        """ get the next epoch for this container """

        # container should be  in the open state
        if self.coh == 0:
            raise DaosApiError("Container needs to be open.")

        epoch = 0
        c_epoch = ctypes.c_uint64(epoch)

        func = self.context.get_function('get-epoch')
        rc = func(self.coh, ctypes.byref(c_epoch), None, None)
        if rc != 0:
            raise DaosApiError("Epoch hold returned non-zero. RC: {0}"
                             .format(rc))

        return c_epoch.value;

    def commit_epoch(self, epoch):
        """ close out an epoch that is done being modified """

        # container should be  in the open state
        if self.coh == 0:
            raise DaosApiError("Container needs to be open.")

        func = self.context.get_function('commit-epoch')
        rc = func(self.coh, epoch, None, None)
        if rc != 0:
            raise DaosApiError("Epoch commit returned non-zero. RC: {0}"
                             .format(rc))

    def consolidate_epochs(self):
        """ consolidate all committed epochs """

        # make sure epoch info is up to date
        self.query()

        func = self.context.get_function('slip-epoch')
        rc = func(self.coh, self.info.es_hce, None, None)
        if rc != 0:
            raise DaosApiError("Epoch slip returned non-zero. RC: {0}"
                             .format(rc))

    def write_an_array_value(self, datalist, dkey, akey, obj=None, rank=None,
			     obj_cls=13):
        """
        Write an array of data to an object.  If an object is not supplied
        a new one is created.  The update occurs in its own epoch and the epoch
        is committed once the update is complete.

        As a simplification I'm expecting the datalist values, dkey and akey
        to be strings.  The datalist values should all be the same size.
        """

        # container should be  in the open state
        if self.coh == 0:
            raise DaosApiError("Container needs to be open.")

        epoch = self.get_new_epoch()
        c_epoch = ctypes.c_uint64(epoch)

        # build a list of tuples where each tuple contains one of the array
        # values and its length in bytes (characters since really expecting
        # strings as the data)
        c_values = []
        for item in datalist:
            c_values.append((ctypes.create_string_buffer(item), len(item)+1))
        c_dkey = ctypes.create_string_buffer(dkey)
        c_akey = ctypes.create_string_buffer(akey)

        # oid can be None in which case a new one is created
        ioreq = IORequest(self.context, self, obj, rank, 2, objtype=obj_cls)
        ioreq.insert_array(c_dkey, c_akey, c_values, c_epoch)
        self.commit_epoch(c_epoch)
        return ioreq.obj, c_epoch.value

    def write_an_obj(self, thedata, size, dkey, akey, obj=None, rank=None,
                     obj_cls=13):
        """
        Write a single value to an object, if an object isn't supplied a new
        one is created.  The update occurs in its own epoch and the epoch is
        committed once the update is complete. The default object class
        specified here, 13, means replication.
        """

        # container should be  in the open state
        if self.coh == 0:
            raise DaosApiError("Container needs to be open.")

        epoch = self.get_new_epoch()
        c_epoch = ctypes.c_uint64(epoch)

        if thedata is not None:
            c_value = ctypes.create_string_buffer(thedata)
        else:
            c_value = None
        c_size = ctypes.c_size_t(size)

        if dkey is None:
            c_dkey = None
        else:
            c_dkey = ctypes.create_string_buffer(dkey)
        if akey is None:
            c_akey = None
        else:
            c_akey = ctypes.create_string_buffer(akey)

        # obj can be None in which case a new one is created
        ioreq = IORequest(self.context, self, obj, rank, objtype=obj_cls)
        ioreq.single_insert(c_dkey, c_akey, c_value, c_size, c_epoch)
        self.commit_epoch(c_epoch)
        return ioreq.obj, c_epoch.value

    def write_multi_akeys(self, dkey, data, obj=None, rank=None):
        """
        Write multiple values to an object, each tagged with a unique akey.
        If an object isn't supplied a new one is created.  The update
        occurs in its own epoch and the epoch is committed once the update is
        complete.

        dkey --the first level key under which all the data is stored.
        data --a list of tuples where each tuple is (akey, data)
        obj  --the object to insert the data into, if None then a new object
               is created.
        rank --the rank to send the update request to
        """

        # container should be  in the open state
        if self.coh == 0:
            raise DaosApiError("Container needs to be open.")

        epoch = self.get_new_epoch()
        c_epoch = ctypes.c_uint64(epoch)

        if dkey is None:
            c_dkey = None
        else:
            c_dkey = ctypes.create_string_buffer(dkey)

        c_data = []
        for tup in data:
            newtup = (ctypes.create_string_buffer(tup[0]),
                      ctypes.create_string_buffer(tup[1]))
            c_data.append(newtup)

        # obj can be None in which case a new one is created
        ioreq = IORequest(self.context, self, obj, rank)

        ioreq.multi_akey_insert(c_dkey, c_data, c_epoch)
        self.commit_epoch(c_epoch)
        return ioreq.obj, c_epoch.value

    def read_an_array(self, rec_count, rec_size, dkey, akey, obj, epoch):
        """
        Reads an array value from the specified object.

        rec_count --number of records (array indicies) to read
        rec_size --each value in the array must be this size

        """

        # container should be  in the open state
        if self.coh == 0:
            raise DaosApiError("Container needs to be open.")

        c_rec_count = ctypes.c_uint(rec_count)
        c_rec_size = ctypes.c_size_t(rec_size)
        c_dkey = ctypes.create_string_buffer(dkey)
        c_akey = ctypes.create_string_buffer(akey)
        c_epoch = ctypes.c_uint64(epoch)

        ioreq = IORequest(self.context, self, obj)
        buf = ioreq.fetch_array(c_dkey, c_akey, c_rec_count,
                                c_rec_size, c_epoch)
        return buf

    def read_multi_akeys(self, dkey, data, obj, epoch):
        """ read multiple values as given by their akeys

        dkey  --which dkey to read from
        obj   --which object to read from
        epoch --which epoch to read from
        data  --a list of tuples (akey, size) where akey is
                the 2nd level key, size is the maximum data
                size for the paired akey

        returns a dictionary of akey:data pairs
        """

        # container should be  in the open state
        if self.coh == 0:
            raise DaosApiError("Container needs to be open.")

        c_dkey = ctypes.create_string_buffer(dkey)
        c_epoch = ctypes.c_uint64(epoch)

        c_data = []
        for tup in data:
            newtup = (ctypes.create_string_buffer(tup[0]),
                      ctypes.c_size_t(tup[1]))
            c_data.append(newtup)

        ioreq = IORequest(self.context, self, obj)
        buf = ioreq.multi_akey_fetch(c_dkey, c_data, c_epoch)
        return buf

    def read_an_obj(self, size, dkey, akey, obj, epoch, test_hints=[]):
        """ read a single value from an object in this container """

        # container should be  in the open state
        if self.coh == 0:
            raise DaosApiError("Container needs to be open.")

        c_size = ctypes.c_size_t(size)
        if dkey is None:
            c_dkey = None
        else:
            c_dkey = ctypes.create_string_buffer(dkey)
        c_akey = ctypes.create_string_buffer(akey)
        c_epoch = ctypes.c_uint64(epoch)

        ioreq = IORequest(self.context, self, obj)
        buf = ioreq.single_fetch(c_dkey, c_akey, size, c_epoch, test_hints)
        return buf

    def local2global(self):
        """ Create a global container handle that can be shared. """

        c_glob = IOV()
        c_glob.iov_len = 0
        c_glob.iov_buf_len = 0
        c_glob.iov_buf = None

        func = self.context.get_function("convert-clocal")
        rc = func(self.coh, ctypes.byref(c_glob))
        if rc != 0:
            raise DaosApiError("Container local2global returned non-zero. RC: {0}"
                             .format(rc))
        # now call it for real
        c_buf = ctypes.create_string_buffer(c_glob.iov_buf_len)
        c_glob.iov_buf = ctypes.cast(c_buf, ctypes.c_void_p)
        rc = func(self.coh, ctypes.byref(c_glob))
        buf = bytearray()
        buf.extend(c_buf.raw)
        return c_glob.iov_len, c_glob.iov_buf_len, buf

    def global2local(self, context, iov_len, buf_len, buf):
        """ Convert a global container handle to a local handle. """

        func = self.context.get_function("convert-cglobal")

        c_glob = IOV()
        c_glob.iov_len = iov_len
        c_glob.iov_buf_len = buf_len
        c_buf = ctypes.create_string_buffer(str(buf))
        c_glob.iov_buf = ctypes.cast(c_buf, ctypes.c_void_p)

        local_handle = ctypes.c_uint64(0)

        rc = func(self.poh, c_glob, ctypes.byref(local_handle))
        if rc != 0:
            raise DaosApiError("Container global2local returned non-zero. RC: {0}"
                             .format(rc))
        self.coh = local_handle
        return local_handle

    def list_attr(self, coh=None, cb_func=None):
        """
        Retrieve a list of user-defined container attribute values.
        Args:
            coh [Optional]:     Container Handler.
            cb_func[Optional]:  To run API in Asynchronous mode.
        return:
            total_size[int]: Total aggregate size of attributes names.
            buffer[String]: Complete aggregated attributes names.
        """
        if coh is not None:
            self.coh = coh
        func = self.context.get_function('list-attr')

        '''
        This is for getting the Aggregate size of all attributes names first
        if it's not passed as a dictionary.
        '''
        sbuf = ctypes.create_string_buffer(100).raw
        t_size = ctypes.pointer(ctypes.c_size_t(100))
        rc = func(self.coh, sbuf, t_size)
        if rc != 0:
            raise DaosApiError("Container List-attr returned non-zero. RC:{0}"
                             .format(rc))
        buf = t_size[0]

        buffer = ctypes.create_string_buffer(buf  + 1).raw
        total_size = ctypes.pointer(ctypes.c_size_t(buf + 1))

        #This will retrieve the list of attributes names.
        if cb_func is None:
            rc = func(self.coh, buffer, total_size, None)
            if rc != 0:
                raise DaosApiError("Container List Attribute returned non-zero.\
                RC: {0}".format(rc))
        else:
            event = DaosEvent()
            params = [self.coh, buffer, total_size, event]
            t = threading.Thread(target=AsyncWorker1,
                                 args=(func,
                                       params,
                                       self.context,
                                       cb_func,
                                       self))
            t.start()
        return total_size[0], buffer

    def set_attr(self, data, coh=None, cb_func=None):
        """
        Set a list of user-defined container attributes.
        Args:
            data[Required]:     Dictionary of Attribute name and value.
            coh [Optional]:     Container Handler
            cb_func[Optional]:  To run API in Asynchronous mode.
        return:
            None
        """
        if coh is not None:
            self.coh = coh

        func = self.context.get_function('set-attr')

        att_names = (ctypes.c_char_p * len(data))(*list(data.keys()))
        names = ctypes.cast(att_names, ctypes.POINTER(ctypes.c_char_p))

        no_of_att = ctypes.c_int(len(data))

        att_values = (ctypes.c_char_p * len(data))(*list(data.values()))
        values = ctypes.cast(att_values, ctypes.POINTER(ctypes.c_char_p))

        size_of_att_val = []
        for key in data.keys():
            if data[key] is not None:
                size_of_att_val.append(len(data[key]))
            else:
                size_of_att_val.append(0)
        sizes = (ctypes.c_size_t * len(data))(*size_of_att_val)

        # the callback function is optional, if not supplied then run the
        # create synchronously, if its there then run it in a thread
        if cb_func is None:
            rc = func(self.coh, no_of_att, names, values, sizes, None)
            if rc != 0:
                raise DaosApiError("Container Set Attribute returned non-zero"
                                 "RC: {0}".format(rc))
        else:
            event = DaosEvent()
            params = [self.coh, no_of_att, names, values, sizes, event]
            t = threading.Thread(target=AsyncWorker1,
                                 args=(func,
                                       params,
                                       self.context,
                                       cb_func,
                                       self))
            t.start()

    def get_attr(self, coh=None, data=None, cb_func=None):
        """
        Retrieve a list of user-defined container attribute values.
        Args:
            coh [Optional]:     Container Handler
            data[Required]:     Dictionary of Attribute name and value.
            cb_func[Optional]:  To run API in Asynchronous mode.
        return:
            buffer[list]:       Requested Attributes value.
        """
        if not data:
            raise DaosApiError("Attribute data should not be blank")

        if coh is not None:
            self.coh = coh

        func = self.context.get_function('get-attr')
        att_names = (ctypes.c_char_p * len(data))(*list(data.keys()))
        names = ctypes.cast(att_names, ctypes.POINTER(ctypes.c_char_p))

        no_of_att = ctypes.c_int(len(data))
        buffers = ctypes.c_char_p * len(data)
        buffer = buffers(*[ctypes.c_char_p(ctypes.create_string_buffer(100).raw)
                           for i in xrange(len(data))])

        size_of_att_val = []
        for key in data.keys():
            if data[key] is not None:
                size_of_att_val.append(len(data[key]))
            else:
                size_of_att_val.append(0)
        sizes = (ctypes.c_size_t * len(data))(*size_of_att_val)

        if cb_func is None:
            rc = func(self.coh, no_of_att, names, ctypes.byref(buffer), sizes,
                      None)
            if rc != 0:
                raise DaosApiError("Container Get Attribute returned non-zero.\
                RC: {0}".format(rc))
        else:
            event = DaosEvent()
            params = [self.coh, no_of_att, names, ctypes.byref(buffer), sizes,
                      event]
            t = threading.Thread(target=AsyncWorker1,
                                 args=(func,
                                       params,
                                       self.context,
                                       cb_func,
                                       self))
            t.start()
        return buffer

class DaosServer(object):
    """Represents a DAOS Server"""

    def __init__(self, context, group, rank):
        """ setup the python pool object, not the real pool. """
        self.context = context
        self.group_name = group
        self.rank = rank

    def kill(self, force):
        """ send a pool creation request to the daos server group """
        c_group = ctypes.create_string_buffer(self.group_name)
        c_force = ctypes.c_int(force)
        c_rank = ctypes.c_uint(self.rank)

        func = self.context.get_function('kill-server')
        rc = func(c_group, c_rank, c_force, None)
        if rc != 0:
            raise DaosApiError("Server kill returned non-zero. RC: {0}"
                             .format(rc))

class DaosContext(object):
    """Provides environment and other info for a DAOS client."""

    def __init__(self, path):
        """ setup the DAOS API and MPI """

        self.libdaos = ctypes.CDLL(path+"libdaos.so.0.0.2",
                                   mode=ctypes.DEFAULT_MODE)
        ctypes.CDLL(path+"libdaos_common.so",
                    mode=ctypes.RTLD_GLOBAL)

        self.libtest = ctypes.CDLL(path+"libdaos_tests.so",
                                   mode=ctypes.DEFAULT_MODE)
        self.libdaos.daos_init()
        # Note: action-subject format
        self.ftable = {
            'add-target'     : self.libdaos.daos_pool_add_tgt,
            'close-cont'     : self.libdaos.daos_cont_close,
            'close-obj'      : self.libdaos.daos_obj_close,
            #'commit-epoch'   : self.libdaos.daos_epoch_commit,
            'connect-pool'   : self.libdaos.daos_pool_connect,
            'convert-cglobal': self.libdaos.daos_cont_global2local,
            'convert-clocal' : self.libdaos.daos_cont_local2global,
            'convert-pglobal': self.libdaos.daos_pool_global2local,
            'convert-plocal' : self.libdaos.daos_pool_local2global,
            'create-cont'    : self.libdaos.daos_cont_create,
            'create-eq'      : self.libdaos.daos_eq_create,
            'create-pool'    : self.libdaos.daos_pool_create,
            'd_log'          : self.libtest.dts_log,
            'destroy-cont'   : self.libdaos.daos_cont_destroy,
            'destroy-eq'     : self.libdaos.daos_eq_destroy,
            'destroy-pool'   : self.libdaos.daos_pool_destroy,
            'disconnect-pool': self.libdaos.daos_pool_disconnect,
            'evict-client'   : self.libdaos.daos_pool_evict,
            'exclude-target' : self.libdaos.daos_pool_exclude,
            'extend-pool'    : self.libdaos.daos_pool_extend,
            'fetch-obj'      : self.libdaos.daos_obj_fetch,
            'generate-oid'   : self.libtest.dts_oid_gen,
            'get-attr'       : self.libdaos.daos_cont_get_attr,
            #'get-epoch'      : self.libdaos.daos_epoch_hold,
            'get-layout'     : self.libdaos.daos_obj_layout_get,
            'init-event'     : self.libdaos.daos_event_init,
            'kill-server'    : self.libdaos.daos_mgmt_svc_rip,
            'kill-target'    : self.libdaos.daos_pool_exclude_out,
            'list-attr'      : self.libdaos.daos_cont_list_attr,
            'open-cont'      : self.libdaos.daos_cont_open,
            'open-obj'       : self.libdaos.daos_obj_open,
            'poll-eq'        : self.libdaos.daos_eq_poll,
            'punch-akeys'    : self.libdaos.daos_obj_punch_akeys,
            'punch-dkeys'    : self.libdaos.daos_obj_punch_dkeys,
            'punch-obj'      : self.libdaos.daos_obj_punch,
            'query-cont'     : self.libdaos.daos_cont_query,
            'query-obj'      : self.libdaos.daos_obj_query,
            'query-pool'     : self.libdaos.daos_pool_query,
            'query-target'   : self.libdaos.daos_pool_query_target,
            'set-attr'       : self.libdaos.daos_cont_set_attr,
            #'slip-epoch'     : self.libdaos.daos_epoch_slip,
            'stop-service'   : self.libdaos.daos_pool_stop_svc,
            'test-event'     : self.libdaos.daos_event_test,
            'update-obj'     : self.libdaos.daos_obj_update}

    def __del__(self):
        """ cleanup the DAOS API """
        self.libdaos.daos_fini()

    def get_function(self, function):
        """ call a function through the API """
        return self.ftable[function]

class DaosLog:

    def __init__(self, context):
        """ setup the log object """
        self.context = context

    def debug(self, msg):
        """ entry point for debug msgs """
        self.daos_log(msg, Logfac.DEBUG)

    def info(self, msg):
        """ entry point for info msgs """
        self.daos_log(msg, Logfac.INFO)

    def warning(self, msg):
        """ entry point for warning msgs """
        self.daos_log(msg, Logfac.WARNING)

    def error(self, msg):
        """ entry point for error msgs """
        self.daos_log(msg, Logfac.ERROR)

    def daos_log(self, msg, level):
        """ write specified message to client daos.log """

        func = self.context.get_function("d_log")

        caller = inspect.getframeinfo(inspect.stack()[2][0])
        caller_func = sys._getframe(1).f_back.f_code.co_name
        filename = os.path.basename(caller.filename)

        c_filename = ctypes.create_string_buffer(filename)
        c_line = ctypes.c_int(caller.lineno)
        c_msg = ctypes.create_string_buffer(msg)
        c_caller_func = ctypes.create_string_buffer(caller_func)
        c_level = ctypes.c_uint64(level)

        func(c_msg, c_filename, c_caller_func, c_line, c_level)

class DaosApiError(Exception):
    """
    DAOS API exception class
    """

if __name__ == '__main__':
    # this file is not intended to be run in normal circumstances
    # this is strictly unit test code here in main, there is a lot
    # of rubbish but it makes it easy to try stuff out as we expand
    # this interface.  Will eventially be removed or formalized.

    """
    try:
        # this works so long as this file is in its usual place
        with open('../../../.build_vars.json') as f:
            data = json.load(f)

        CONTEXT = DaosContext(data['PREFIX'] + '/lib/')
        print ("initialized!!!\n")

        POOL = DaosPool(CONTEXT)
        tgt_list = [1]
        POOL.create(448, os.getuid(), os.getgid(), 1024 * 1024 * 1024,
                    b'daos_server')
        time.sleep(2)
        print ("Pool create called\n")
        print ("uuid is " + POOL.get_uuid_str())

        #time.sleep(5)
        print ("handle before connect {0}\n".format(POOL.handle))

        POOL.connect(1 << 1)

        print ("Main: handle after connect {0}\n".format(POOL.handle))

        CONTAINER = DaosContainer(CONTEXT)
        CONTAINER.create(POOL.handle)

        print ("container created {}".format(CONTAINER.get_uuid_str()))

        #POOL.pool_svc_stop();
        #POOL.pool_query()

        time.sleep(5)

        CONTAINER.open()
        print ("container opened {}".format(CONTAINER.get_uuid_str()))

        time.sleep(5)

        CONTAINER.query()
        print ("Epoch highest committed: {}".format(CONTAINER.info.es_hce))

        thedata = "a string that I want to stuff into an object"
        size = 45
        dkey = "this is the dkey"
        akey = "this is the akey"

        obj, epoch = CONTAINER.write_an_obj(thedata, size, dkey, akey, None, 5)
        print ("data write finished with epoch {}".format(epoch))

        obj.get_layout()
        for i in obj.tgt_rank_list:
            print ("rank for obj:{}".format(i))

        time.sleep(5)

        thedata2 = CONTAINER.read_an_obj(size, dkey, akey, obj, epoch)
        print (repr(thedata2.value))

        thedata3 = "a different string that I want to stuff into an object"
        size = 55
        dkey2 = "this is the dkey"
        akey2 = "this is the akey"

        obj2, epoch2 = CONTAINER.write_an_obj(thedata3, size, dkey2,
                                              akey2, obj, 4)
        print ("data write finished, in epoch {}".format(epoch2))

        obj2.get_layout()

        time.sleep(5)

        thedata4 = CONTAINER.read_an_obj(size, dkey2, akey2, obj2, epoch2)
        print (repr(thedata4.value))

        thedata5 = CONTAINER.read_an_obj(size, dkey2, akey2, obj, epoch)
        print (repr(thedata5.value))

        data = {"First":  "1111111111",
                "Second": "22222222222222222222",
                "Third":  "333333333333333333333333333333"}
        print ("=====Set Attr =====")
        CONTAINER.set_attr(data = data)
        print ("=====List Attr =====")
        # commenting out below line, list_attr has no data param
        # size, buf = CONTAINER.list_attr(data = data)
        # print size, buf
        print ("=====Get Attr =====")
        val = CONTAINER.get_attr(data = data)
        for i in range(0, len(data)):
            print(val[i])

        CONTAINER.close()
        print ("container closed {}".format(CONTAINER.get_uuid_str()))

        time.sleep(15)

        CONTAINER.destroy(1)

        print ("container destroyed")

        #POOL.disconnect(rubbish)
        #POOL.disconnect()
        #print ("Main past disconnect\n")

        #time.sleep(5)

        #tgts = [2]
        #POOL.exclude(tgts, rubbish)
        #POOL.exclude_out(tgts, rubbish)
        #POOL.exclude_out(tgts)
        #print ("Main past exclude\n")

        #POOL.evict(rubbish)

        #time.sleep(5)

        #POOL.tgt_add(tgts, rubbish)

        #print ("Main past tgt_add\n")

        #POOL.destroy(1)
        #print ("Pool destroyed")

        #SERVICE = DaosServer(CONTEXT, b'daos_server', 5)
        #SERVICE.kill(1)
        #print ("server killed!\n")

    except Exception as EXCEP:
        print ("Something horrible happened\n")
        print (traceback.format_exc())
        print (EXCEP)
    """

    print("running")
    raise DaosApiError("hit error, all good")
