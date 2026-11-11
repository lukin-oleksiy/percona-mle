/* Copyright (c) 2026 Percona LLC and/or its affiliates. All rights
   reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#include "mle/launcher_protocol.h"

namespace mle::launcher {

void put(Writer &w, const std::vector<std::string> &list) {
  w.u32(static_cast<uint32_t>(list.size()));
  for (const std::string &s : list) w.str(s);
}

bool get(Reader &r, std::vector<std::string> *list) {
  uint32_t n;
  if (!r.count(&n, 4)) return false;
  list->clear();
  list->resize(n);
  for (std::string &s : *list)
    if (!r.str(&s)) return false;
  return true;
}

void put(Writer &w, const ClassConfig &c) {
  w.str(c.name);
  w.str(c.executable);
  put(w, c.args);
  put(w, c.env);
  put(w, c.read_paths);
  put(w, c.exec_paths);
  put(w, c.hidden_paths);
  w.u64(c.memory_max);
  w.u32(c.pids_max);
  w.u32(c.nofile);
  w.u8(c.core_dumps ? 1 : 0);
  w.u32(c.pool_size);
  w.u8(c.inherit_stderr ? 1 : 0);
  w.u8(c.insecure_without_landlock ? 1 : 0);
}

bool get(Reader &r, ClassConfig *c) {
  return r.str(&c->name) && r.str(&c->executable) && get(r, &c->args) &&
         get(r, &c->env) && get(r, &c->read_paths) && get(r, &c->exec_paths) &&
         get(r, &c->hidden_paths) &&
         r.u64(&c->memory_max) && r.u32(&c->pids_max) && r.u32(&c->nofile) &&
         r.flag(&c->core_dumps) && r.u32(&c->pool_size) &&
         r.flag(&c->inherit_stderr) && r.flag(&c->insecure_without_landlock);
}

void put(Writer &w, const ClassStats &s) {
  w.u32(s.idle);
  w.u64(s.spawned);
  w.u64(s.handed_out);
  w.u64(s.pool_deaths);
  w.u32(s.consecutive_pool_deaths);
  w.str(s.last_error);
}

bool get(Reader &r, ClassStats *s) {
  return r.u32(&s->idle) && r.u64(&s->spawned) && r.u64(&s->handed_out) &&
         r.u64(&s->pool_deaths) && r.u32(&s->consecutive_pool_deaths) &&
         r.str(&s->last_error);
}

}  // namespace mle::launcher
