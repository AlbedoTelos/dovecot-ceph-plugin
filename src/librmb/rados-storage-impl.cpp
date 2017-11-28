// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Copyright (c) 2017 Tallence AG and the authors
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 */

#include "rados-storage-impl.h"

#include <algorithm>
#include <list>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <rados/librados.hpp>
#include "encoding.h"
#include "limits.h"

using std::pair;
using std::string;

using ceph::bufferlist;

using librmb::RadosStorageImpl;

#define DICT_USERNAME_SEPARATOR '/'
const char *RadosStorageImpl::CFG_OSD_MAX_WRITE_SIZE = "osd_max_write_size";

RadosStorageImpl::RadosStorageImpl(RadosCluster *_cluster) {
  cluster = _cluster;
  max_write_size = 10;
}

RadosStorageImpl::~RadosStorageImpl() {}

int RadosStorageImpl::split_buffer_and_exec_op(const char *buffer, size_t buffer_length,
                                               RadosMailObject *current_object,
                                               librados::ObjectWriteOperation *write_op_xattr, uint64_t max_write) {
  if (!cluster->is_connected()) {
    return -1;
  }

  size_t write_buffer_size = buffer_length;
  int ret_val = 0;
  assert(max_write > 0);

  int rest = write_buffer_size % max_write;
  int div = write_buffer_size / max_write + (rest > 0 ? 1 : 0);
  for (int i = 0; i < div; i++) {
    int offset = i * max_write;

    librados::ObjectWriteOperation *op = i == 0 ? write_op_xattr : new librados::ObjectWriteOperation();

    uint64_t length = max_write;
    if (buffer_length < ((i + 1) * length)) {
      length = rest;
    }
    const char *buf = buffer + offset;
    librados::bufferlist tmp_buffer;
    tmp_buffer.append(buf, length);
    op->write(offset, tmp_buffer);

    librados::AioCompletion *completion = librados::Rados::aio_create_completion();

    (*current_object->get_completion_op_map())[completion] = op;

    ret_val = get_io_ctx().aio_operate(current_object->get_oid(), completion, op);
    if (ret_val < 0) {
      break;
    }
  }

  return ret_val;
}
int RadosStorageImpl::save_mail(const std::string &oid, librados::bufferlist &buffer) {
  return get_io_ctx().write_full(oid, buffer);
}

int RadosStorageImpl::read_mail(const std::string &oid, librados::bufferlist *buffer) {
  if (!cluster->is_connected()) {
    return -1;
  }
  size_t max = INT_MAX;
  return get_io_ctx().read(oid, *buffer, max, 0);
}

int RadosStorageImpl::load_metadata(RadosMailObject *mail) {
  int ret = -1;
  if (!cluster->is_connected()) {
    return ret;
  }

  if (mail != nullptr) {
    if (mail->get_metadata()->size() == 0) {
      ret = get_io_ctx().getxattrs(mail->get_oid(), *mail->get_metadata());
    } else {
      ret = 0;
    }
  }
  return ret;
}
int RadosStorageImpl::update_extended_metadata(std::string &oid, RadosMetadata *metadata) {
  int ret = -1;
  if (!cluster->is_connected()) {
    return ret;
  }

  if (metadata != nullptr) {
    std::map<std::string, bufferlist> map;
    map.insert(pair<string, bufferlist>(metadata->key, metadata->bl));
    ret = get_io_ctx().omap_set(oid, map);
  }
  return ret;
}
int RadosStorageImpl::remove_extended_metadata(std::string &oid, std::string &key) {
  if (!cluster->is_connected()) {
    return -1;
  }

  std::set<std::string> keys;
  keys.insert(key);
  return get_io_ctx().omap_rm_keys(oid, keys);
}
int RadosStorageImpl::load_extended_metadata(std::string &oid, std::set<std::string> &keys,
                                             std::map<std::string, ceph::bufferlist> *metadata) {
  if (!cluster->is_connected()) {
    return -1;
  }
  return get_io_ctx().omap_get_vals_by_keys(oid, keys, metadata);
}

int RadosStorageImpl::set_metadata(const std::string &oid, const RadosMetadata &xattr) {
  if (!cluster->is_connected()) {
    return -1;
  }
  ceph::bufferlist bl = xattr.bl;
  return get_io_ctx().setxattr(oid, xattr.key.c_str(), bl);
}

int RadosStorageImpl::delete_mail(RadosMailObject *mail) {
  int ret = -1;

  if (cluster->is_connected() && mail != nullptr) {
    ret = delete_mail(mail->get_oid());
  }
  return ret;
}
int RadosStorageImpl::delete_mail(std::string oid) {
  int ret = -1;
  if (!cluster->is_connected()) {
    return -1;
  }

  if (!oid.empty()) {
    ret = get_io_ctx().remove(oid);
  }
  return ret;
}

int RadosStorageImpl::aio_operate(librados::IoCtx *io_ctx_, const std::string &oid, librados::AioCompletion *c,
                                  librados::ObjectWriteOperation *op) {
  if (!cluster->is_connected()) {
    return -1;
  }

  if (io_ctx_ != nullptr) {
    return io_ctx_->aio_operate(oid, c, op);
  } else {
    return get_io_ctx().aio_operate(oid, c, op);
  }
}

int RadosStorageImpl::stat_mail(const std::string &oid, uint64_t *psize, time_t *pmtime) {
  if (!cluster->is_connected()) {
    return -1;
  }

  return get_io_ctx().stat(oid, psize, pmtime);
}
void RadosStorageImpl::set_namespace(const std::string &_nspace) {
  get_io_ctx().set_namespace(_nspace);
  this->nspace = _nspace;
}

librados::NObjectIterator RadosStorageImpl::find_mails(const RadosMetadata *attr) {
  if (!cluster->is_connected()) {
    return librados::NObjectIterator::__EndObjectIterator;
  }
  if (attr != nullptr) {
    std::string filter_name = PLAIN_FILTER_NAME;
    ceph::bufferlist filter_bl;

    encode(filter_name, filter_bl);
    encode("_" + attr->key, filter_bl);
    encode(attr->bl.to_str(), filter_bl);

    return get_io_ctx().nobjects_begin(filter_bl);
  } else {
    return get_io_ctx().nobjects_begin();
  }
}

librados::IoCtx &RadosStorageImpl::get_io_ctx() { return io_ctx; }

int RadosStorageImpl::open_connection(const string &poolname) {
  if (cluster->init() < 0) {
    return -1;
  }
  // pool exists? else create
  int err = cluster->io_ctx_create(poolname, &io_ctx);
  if (err < 0) {
    return err;
  }
  string max_write_size_str;
  err = cluster->get_config_option(RadosStorageImpl::CFG_OSD_MAX_WRITE_SIZE, &max_write_size_str);
  if (err < 0) {
    return err;
  }
  max_write_size = std::stoi(max_write_size_str);

  return 0;
}

bool RadosStorageImpl::wait_for_write_operations_complete(
    std::map<librados::AioCompletion *, librados::ObjectWriteOperation *> *completion_op_map) {
  bool failed = false;
  for (std::map<librados::AioCompletion *, librados::ObjectWriteOperation *>::iterator map_it =
           completion_op_map->begin();
       map_it != completion_op_map->end(); ++map_it) {
    map_it->first->wait_for_complete_and_cb();
    failed = map_it->first->get_return_value() < 0 || failed ? true : false;
    // clean up
    map_it->first->release();
    map_it->second->remove();
    delete map_it->second;
  }
  return failed;
}

bool RadosStorageImpl::wait_for_rados_operations(const std::vector<librmb::RadosMailObject *> &object_list) {
  bool ctx_failed = false;
    // wait for all writes to finish!
    // imaptest shows it's possible that begin -> continue -> finish cycle is invoked several times before
    // rbox_transaction_save_commit_pre is called.
  for (std::vector<librmb::RadosMailObject *>::const_iterator it_cur_obj = object_list.begin();
       it_cur_obj != object_list.end(); ++it_cur_obj) {
    // if we come from copy mail, there is no operation to wait for.
    if ((*it_cur_obj)->has_active_op()) {
      bool op_failed = wait_for_write_operations_complete((*it_cur_obj)->get_completion_op_map());

      ctx_failed = ctx_failed ? ctx_failed : op_failed;
      (*it_cur_obj)->get_completion_op_map()->clear();
      (*it_cur_obj)->set_active_op(false);
    }
  }
  return ctx_failed;
}

bool RadosStorageImpl::update_metadata(std::string oid, std::list<RadosMetadata> &to_update) {
  if (!cluster->is_connected()) {
    return false;
  }
  librados::ObjectWriteOperation write_op;
  librados::AioCompletion *completion = librados::Rados::aio_create_completion();
  // update metadata
  for (std::list<RadosMetadata>::iterator it = to_update.begin(); it != to_update.end(); ++it) {
    write_op.setxattr((*it).key.c_str(), (*it).bl);
  }
  int ret = aio_operate(&io_ctx, oid, completion, &write_op);
  completion->wait_for_complete();
  completion->release();
  return ret == 0;
}

// assumes that destination io ctx is current io_ctx;
bool RadosStorageImpl::move(std::string &src_oid, const char *src_ns, std::string &dest_oid, const char *dest_ns,
                            std::list<RadosMetadata> &to_update, bool delete_source) {
  if (!cluster->is_connected()) {
    return false;
  }

  librados::ObjectWriteOperation write_op;
  librados::IoCtx src_io_ctx, dest_io_ctx;

  librados::AioCompletion *completion = librados::Rados::aio_create_completion();

  // destination io_ctx is current io_ctx
  dest_io_ctx = io_ctx;

  if (strcmp(src_ns, dest_ns) != 0) {
    src_io_ctx.dup(dest_io_ctx);
    src_io_ctx.set_namespace(src_ns);
    dest_io_ctx.set_namespace(dest_ns);
    write_op.copy_from(src_oid, src_io_ctx, src_io_ctx.get_last_version());

    } else {
      src_io_ctx = dest_io_ctx;
    }

    // because we create a copy, save date needs to be updated
    // as an alternative we could use &ctx->data.save_date here if we save it to xattribute in write_metadata
    // and restore it in read_metadata function. => save_date of copy/move will be same as source.
    // write_op.mtime(&ctx->data.save_date);
    time_t save_time = time(NULL);
    write_op.mtime(&save_time);

    // update metadata
    for (std::list<RadosMetadata>::iterator it = to_update.begin(); it != to_update.end(); ++it) {
      write_op.setxattr((*it).key.c_str(), (*it).bl);
    }

    int ret = aio_operate(&dest_io_ctx, dest_oid, completion, &write_op);
    completion->wait_for_complete();
    completion->release();

    if (delete_source && strcmp(src_ns, dest_ns) != 0) {
      src_io_ctx.remove(src_oid);
    }
    // reset io_ctx
    dest_io_ctx.set_namespace(dest_ns);
    return ret == 0;
}

// assumes that destination io ctx is current io_ctx;
bool RadosStorageImpl::copy(std::string &src_oid, const char *src_ns, std::string &dest_oid, const char *dest_ns,
                            std::list<RadosMetadata> &to_update) {
  if (!cluster->is_connected()) {
    return false;
  }

  librados::ObjectWriteOperation write_op;
  librados::IoCtx src_io_ctx, dest_io_ctx;

  librados::AioCompletion *completion = librados::Rados::aio_create_completion();

  // destination io_ctx is current io_ctx
  dest_io_ctx = io_ctx;

  if (strcmp(src_ns, dest_ns) != 0) {
    src_io_ctx.dup(dest_io_ctx);
    src_io_ctx.set_namespace(src_ns);
    dest_io_ctx.set_namespace(dest_ns);
    } else {
      src_io_ctx = dest_io_ctx;
    }
    write_op.copy_from(src_oid, src_io_ctx, src_io_ctx.get_last_version());

    // because we create a copy, save date needs to be updated
    // as an alternative we could use &ctx->data.save_date here if we save it to xattribute in write_metadata
    // and restore it in read_metadata function. => save_date of copy/move will be same as source.
    // write_op.mtime(&ctx->data.save_date);
    time_t save_time = time(NULL);
    write_op.mtime(&save_time);

    // update metadata
    for (std::list<RadosMetadata>::iterator it = to_update.begin(); it != to_update.end(); ++it) {
      write_op.setxattr((*it).key.c_str(), (*it).bl);
    }

    int ret = aio_operate(&dest_io_ctx, dest_oid, completion, &write_op);
    ret = completion->wait_for_complete();
    completion->release();
    // reset io_ctx
    dest_io_ctx.set_namespace(dest_ns);
    return ret == 0;
}

// if save_async = true, don't forget to call wait_for_rados_operations e.g. wait_for_write_operations_complete
// to wait for completion and free resources.
bool RadosStorageImpl::save_mail(RadosMailObject *mail, bool &save_async) {
  if (!cluster->is_connected()) {
    return false;
  }
  // delete write_op_xattr is called after operation completes (wait_for_rados_operations)
  librados::ObjectWriteOperation *write_op_xattr = new librados::ObjectWriteOperation();
  int ret = -1;
  // set metadata
  for (std::map<std::string, librados::bufferlist>::iterator it = mail->get_metadata()->begin();
       it != mail->get_metadata()->end(); ++it) {
    write_op_xattr->setxattr(it->first.c_str(), it->second);
  }

  if (mail->get_extended_metadata()->size() > 0) {
    write_op_xattr->omap_set(*mail->get_extended_metadata());
  }

  write_op_xattr->mtime(mail->get_rados_save_date());
  ret = split_buffer_and_exec_op(reinterpret_cast<const char *>(mail->get_mail_buffer_content_ptr()),
                                 mail->get_mail_size(), mail, write_op_xattr, get_max_write_size_bytes());
  mail->set_active_op(true);
  if (!save_async) {
    std::vector<librmb::RadosMailObject *> objects;
    objects.push_back(mail);
    return wait_for_rados_operations(objects);
  }
  if (ret != 0) {
    write_op_xattr->remove();
    delete write_op_xattr;
    mail->set_active_op(false);
  }

  return ret == 0;
}
librmb::RadosMailObject *RadosStorageImpl::alloc_mail_object() { return new librmb::RadosMailObject(); }
void RadosStorageImpl::free_mail_object(librmb::RadosMailObject *mail) {
  delete mail;
  mail = nullptr;
}
