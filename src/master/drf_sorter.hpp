/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __DRF_SORTER_HPP__
#define __DRF_SORTER_HPP__

#include <set>
#include <string>

#include <mesos/resources.hpp>

#include <stout/hashmap.hpp>

#include "master/sorter.hpp"


namespace mesos {
namespace internal {
namespace master {
namespace allocator {

struct Client
{
  std::string name;
  double share;
};


struct DRFComparator
{
  virtual ~DRFComparator() {}
  virtual bool operator () (const Client& client1,
                            const Client& client2);
};


typedef std::set<Client, DRFComparator> drfSet;


class DRFSorter : public Sorter
{
public:
  virtual ~DRFSorter() {}

  virtual void add(const std::string& name, double weight = 1);

  virtual void remove(const std::string& name);

  virtual void activate(const std::string& name);

  virtual void deactivate(const std::string& name);

  virtual void allocated(const std::string& name,
                         const Resources& resources);

  virtual void unallocated(const std::string& name,
                           const Resources& resources);

  virtual Resources allocation(const std::string& name);

  virtual void add(const Resources& resources);

  virtual void remove(const Resources& resources);

  virtual std::list<std::string> sort();

  virtual bool contains(const std::string& name);

  virtual int count();

private:
  // Recalculates the share for the client and moves
  // it in 'clients' accordingly.
  void update(const std::string& name);

  // Returns the dominant resource share for the client.
  double calculateShare(const std::string& name);

  // Returns an iterator to the specified client, if
  // it exists in this Sorter.
  std::set<Client, DRFComparator>::iterator find(const std::string& name);

  // If true, start() will recalculate all shares.
  bool dirty;

  // A set of Clients (names and shares) sorted by share.
  std::set<Client, DRFComparator> clients;

  // Maps client names to the resources they have been allocated.
  hashmap<std::string, Resources> allocations;

  // Maps client names to the weights that should be applied to their shares.
  hashmap<std::string, double> weights;

  // Total resources.
  Resources resources;
};

} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __DRF_SORTER_HPP__
