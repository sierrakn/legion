/* Copyright 2016 Stanford University, NVIDIA Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "legion.h"
#include "runtime.h"
#include "legion_ops.h"
#include "legion_tasks.h"
#include "region_tree.h"
#include "legion_spy.h"
#include "legion_profiling.h"
#include "legion_instances.h"
#include "legion_views.h"

namespace Legion {
  namespace Internal {

    LEGION_EXTERN_LOGGER_DECLARATIONS

    /////////////////////////////////////////////////////////////
    // Layout Description 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    LayoutDescription::LayoutDescription(FieldSpaceNode *own,
                                         const FieldMask &mask,
                                         LayoutConstraints *con,
                                   const std::vector<unsigned> &mask_index_map,
                                     const std::vector<CustomSerdezID> &serdez,
                    const std::vector<std::pair<FieldID,size_t> > &field_sizes)
      : allocated_fields(mask), constraints(con), owner(own) 
    //--------------------------------------------------------------------------
    {
      layout_lock = Reservation::create_reservation();
      field_infos.resize(field_sizes.size());
      // Switch data structures from layout by field order to order
      // of field locations in the bit mask
#ifdef DEBUG_HIGH_LEVEL
      assert(mask_index_map.size() == 
                size_t(FieldMask::pop_count(allocated_fields)));
#endif
#ifndef NEW_INSTANCE_CREATION
      std::vector<size_t> offsets(field_sizes.size(),0);
      for (unsigned idx = 1; idx < field_sizes.size(); idx++)
        offsets[idx] = offsets[idx-1] + field_sizes[idx-1].second;
#endif
      for (unsigned idx = 0; idx < mask_index_map.size(); idx++)
      {
        // This gives us the index in the field ordered data structures
        unsigned index = mask_index_map[idx];
        FieldID fid = field_sizes[index].first;
        field_indexes[fid] = idx;
#ifdef NEW_INSTANCE_CREATION
        Domain::CopySrcDstFieldInfo &info = field_infos[idx];
        info.field_id = fid;
#else
        Domain::CopySrcDstField &info = field_infos[idx];
        info.offset = offsets[index];
        info.size = field_sizes[index].second;
#endif
        info.serdez_id = serdez[index];
      }
    }

    //--------------------------------------------------------------------------
    LayoutDescription::LayoutDescription(const FieldMask &mask,
                                         LayoutConstraints *con)
      : allocated_fields(mask), constraints(con), owner(NULL)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    LayoutDescription::LayoutDescription(const LayoutDescription &rhs)
      : allocated_fields(rhs.allocated_fields), 
        constraints(rhs.constraints), owner(rhs.owner)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    LayoutDescription::~LayoutDescription(void)
    //--------------------------------------------------------------------------
    {
      comp_cache.clear();
      layout_lock.destroy_reservation();
      layout_lock = Reservation::NO_RESERVATION;
    }

    //--------------------------------------------------------------------------
    LayoutDescription& LayoutDescription::operator=(
                                                   const LayoutDescription &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    void* LayoutDescription::operator new(size_t count)
    //--------------------------------------------------------------------------
    {
      return legion_alloc_aligned<LayoutDescription,true/*bytes*/>(count);
    }

    //--------------------------------------------------------------------------
    void LayoutDescription::operator delete(void *ptr)
    //--------------------------------------------------------------------------
    {
      free(ptr);
    }

    //--------------------------------------------------------------------------
    void LayoutDescription::log_instance_layout(PhysicalInstance inst) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(Runtime::legion_spy_enabled);
#endif
      std::vector<FieldID> fields;  
      owner->get_field_ids(allocated_fields, fields);
      for (std::vector<FieldID>::const_iterator it = fields.begin();
            it != fields.end(); it++)
        LegionSpy::log_physical_instance_field(inst.id, *it);
    }

    //--------------------------------------------------------------------------
    void LayoutDescription::compute_copy_offsets(const FieldMask &copy_mask,
                                                 PhysicalInstance instance,
                                   std::vector<Domain::CopySrcDstField> &fields)
    //--------------------------------------------------------------------------
    {
      uint64_t hash_key = copy_mask.get_hash_key();
      bool found_in_cache = false;
      FieldMask compressed;
      // First check to see if we've memoized this result 
      {
        AutoLock o_lock(layout_lock,1,false/*exclusive*/);
        std::map<LEGION_FIELD_MASK_FIELD_TYPE,
                 LegionList<std::pair<FieldMask,FieldMask> >::aligned>::
                   const_iterator finder = comp_cache.find(hash_key);
        if (finder != comp_cache.end())
        {
          for (LegionList<std::pair<FieldMask,FieldMask> >::aligned::
                const_iterator it = finder->second.begin(); 
                it != finder->second.end(); it++)
          {
            if (it->first == copy_mask)
            {
              found_in_cache = true;
              compressed = it->second;
              break;
            }
          }
        }
      }
      if (!found_in_cache)
      {
        compressed = copy_mask;
        compress_mask<STATIC_LOG2(MAX_FIELDS)>(compressed, allocated_fields);
        // Save the result in the cache, duplicates from races here are benign
        AutoLock o_lock(layout_lock);
        comp_cache[hash_key].push_back(
            std::pair<FieldMask,FieldMask>(copy_mask,compressed));
      }
      // It is absolutely imperative that these infos be added in
      // the order in which they appear in the field mask so that 
      // they line up in the same order with the source/destination infos
      // (depending on the calling context of this function
      int pop_count = FieldMask::pop_count(compressed);
#ifdef DEBUG_HIGH_LEVEL
      assert(pop_count == FieldMask::pop_count(copy_mask));
#endif
      unsigned offset = fields.size();
      fields.resize(offset + pop_count);
      int next_start = 0;
      for (int idx = 0; idx < pop_count; idx++)
      {
        int index = compressed.find_next_set(next_start);
        Domain::CopySrcDstField &field = fields[offset+idx];
        field = field_infos[index];
        // Our field infos are annonymous so specify the instance now
        field.inst = instance;
        // We'll start looking again at the next index after this one
        next_start = index + 1;
      }
    }

    //--------------------------------------------------------------------------
    template<unsigned LOG2MAX>
    /*static*/ void LayoutDescription::compress_mask(FieldMask &x, FieldMask m)
    //--------------------------------------------------------------------------
    {
      FieldMask mk, mp, mv, t;
      // See hacker's delight 7-4
      x = x & m;
      mk = ~m << 1;
      for (unsigned i = 0; i < LOG2MAX; i++)
      {
        mp = mk ^ (mk << 1);
        for (unsigned idx = 1; idx < LOG2MAX; idx++)
          mp = mp ^ (mp << (1 << idx));
        mv = mp & m;
        m = (m ^ mv) | (mv >> (1 << i));
        t = x & mv;
        x = (x ^ t) | (t >> (1 << i));
        mk = mk & ~mp;
      }
    }

    //--------------------------------------------------------------------------
    void LayoutDescription::compute_copy_offsets(FieldID fid, 
        PhysicalInstance instance, std::vector<Domain::CopySrcDstField> &fields)
    //--------------------------------------------------------------------------
    {
      std::map<FieldID,unsigned>::const_iterator finder = 
        field_indexes.find(fid);
#ifdef DEBUG_HIGH_LEVEL
      assert(finder != field_indexes.end());
#endif
      fields.push_back(field_infos[finder->second]);
      // Since instances are annonymous in layout descriptions we
      // have to fill them in when we add the field info
      fields.back().inst = instance;
    }

    //--------------------------------------------------------------------------
    void LayoutDescription::compute_copy_offsets(
                                   const std::vector<FieldID> &copy_fields, 
                                   PhysicalInstance instance,
                                   std::vector<Domain::CopySrcDstField> &fields)
    //--------------------------------------------------------------------------
    {
      unsigned offset = fields.size();
      fields.resize(offset + copy_fields.size());
      for (unsigned idx = 0; idx < copy_fields.size(); idx++)
      {
        std::map<FieldID,unsigned>::const_iterator
          finder = field_indexes.find(copy_fields[idx]);
#ifdef DEBUG_HIGH_LEVEL
        assert(finder != field_indexes.end());
#endif
        Domain::CopySrcDstField &info = fields[offset+idx];
        info = field_infos[finder->second];
        // Since instances are annonymous in layout descriptions we
        // have to fill them in when we add the field info
        info.inst = instance;
      }
    }

    //--------------------------------------------------------------------------
    bool LayoutDescription::has_field(FieldID fid) const
    //--------------------------------------------------------------------------
    {
      return (field_indexes.find(fid) != field_indexes.end());
    }

    //--------------------------------------------------------------------------
    void LayoutDescription::has_fields(std::map<FieldID,bool> &to_test) const
    //--------------------------------------------------------------------------
    {
      for (std::map<FieldID,bool>::iterator it = to_test.begin();
            it != to_test.end(); it++)
      {
        if (field_indexes.find(it->first) != field_indexes.end())
          it->second = true;
        else
          it->second = false;
      }
    }

    //--------------------------------------------------------------------------
    void LayoutDescription::remove_space_fields(std::set<FieldID> &filter) const
    //--------------------------------------------------------------------------
    {
      std::vector<FieldID> to_remove;
      for (std::set<FieldID>::const_iterator it = filter.begin();
            it != filter.end(); it++)
      {
        if (field_indexes.find(*it) != field_indexes.end())
          to_remove.push_back(*it);
      }
      if (!to_remove.empty())
      {
        for (std::vector<FieldID>::const_iterator it = to_remove.begin();
              it != to_remove.end(); it++)
          filter.erase(*it);
      }
    }

    //--------------------------------------------------------------------------
    const Domain::CopySrcDstField& LayoutDescription::find_field_info(
                                                              FieldID fid) const
    //--------------------------------------------------------------------------
    {
      std::map<FieldID,unsigned>::const_iterator finder = 
        field_indexes.find(fid);
#ifdef DEBUG_HIGH_LEVEL
      assert(finder != field_indexes.end());
#endif
      return field_infos[finder->second];
    }

    //--------------------------------------------------------------------------
    size_t LayoutDescription::get_total_field_size(void) const
    //--------------------------------------------------------------------------
    {
      size_t result = 0;
      // Add up all the field sizes
      for (std::vector<Domain::CopySrcDstField>::const_iterator it = 
            field_infos.begin(); it != field_infos.end(); it++)
      {
        result += it->size;
      }
      return result;
    }

    //--------------------------------------------------------------------------
    void LayoutDescription::get_fields(std::vector<FieldID>& fields) const
    //--------------------------------------------------------------------------
    {
      // order field ids by their offsets by inserting them to std::map
      std::map<unsigned, FieldID> offsets;
      for (std::map<FieldID,unsigned>::const_iterator it = 
            field_indexes.begin(); it != field_indexes.end(); it++)
      {
        const Domain::CopySrcDstField &info = field_infos[it->second];
        offsets[info.offset] = it->first;
      }
      for (std::map<unsigned, FieldID>::const_iterator it = offsets.begin();
           it != offsets.end(); ++it)
        fields.push_back(it->second);
    }

    //--------------------------------------------------------------------------
    void LayoutDescription::compute_destroyed_fields(
             std::vector<PhysicalInstance::DestroyedField> &serdez_fields) const
    //--------------------------------------------------------------------------
    {
      // See if we have any special fields which need serdez deletion
      for (std::vector<Domain::CopySrcDstField>::const_iterator it = 
            field_infos.begin(); it != field_infos.end(); it++)
      {
        if (it->serdez_id > 0)
          serdez_fields.push_back(PhysicalInstance::DestroyedField(it->offset, 
                                                    it->size, it->serdez_id));
      }
    }

    //--------------------------------------------------------------------------
    bool LayoutDescription::match_layout(
                         const LayoutConstraintSet &candidate_constraints) const
    //--------------------------------------------------------------------------
    {
      // Layout descriptions are always complete, so just check for conflicts
      if (constraints->conflicts(candidate_constraints))
        return false;
      // If they don't conflict they have to be the same
      return true;
    }

    //--------------------------------------------------------------------------
    bool LayoutDescription::match_layout(const LayoutDescription *layout) const
    //--------------------------------------------------------------------------
    {
      if (layout->allocated_fields != allocated_fields)
        return false;
      // Layout descriptions are always complete so just check for conflicts
      if (constraints->conflicts(layout->constraints))
        return false;
      // If they don't conflict they have to be the same
      return true;
    }

    //--------------------------------------------------------------------------
    void LayoutDescription::set_descriptor(FieldDataDescriptor &desc,
                                           FieldID fid) const
    //--------------------------------------------------------------------------
    {
      std::map<FieldID,unsigned>::const_iterator finder = 
        field_indexes.find(fid);
#ifdef DEBUG_HIGH_LEVEL
      assert(finder != field_indexes.end());
#endif
      const Domain::CopySrcDstField &info = field_infos[finder->second];
      desc.field_offset = info.offset;
      desc.field_size = info.size;
    }

    //--------------------------------------------------------------------------
    void LayoutDescription::pack_layout_description(Serializer &rez,
                                                    AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      RezCheck z(rez);
      // Do a quick check to see if the target already has the layout
      // We don't need to hold a lock here since if we lose the race
      // we will just send the layout twice and everything will be
      // resolved on the far side
      if (known_nodes.contains(target))
        rez.serialize<bool>(true);
      else
        rez.serialize<bool>(false);
      // If it is already on the remote node, then we only
      // need to the necessary information to identify it
      DistributedID constraint_did = constraints->send_constraints(target);
      rez.serialize(constraint_did);
      rez.serialize(allocated_fields);
    }

    //--------------------------------------------------------------------------
    void LayoutDescription::unpack_layout_description(Deserializer &derez)
    //--------------------------------------------------------------------------
    {
      size_t num_fields;
      derez.deserialize(num_fields);
      for (unsigned idx = 0; idx < num_fields; idx++)
      {
        FieldID fid;
        derez.deserialize(fid);
        unsigned index = owner->get_field_index(fid);
        field_indexes[index] = fid;
        Domain::CopySrcDstField &info = field_infos[fid];
        derez.deserialize(info.offset);
        derez.deserialize(info.size);
        derez.deserialize(info.serdez_id);
      }
    }

    //--------------------------------------------------------------------------
    void LayoutDescription::update_known_nodes(AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      // Hold the lock to get serial access to this data structure
      AutoLock l_lock(layout_lock);
      known_nodes.add(target);
    }

    //--------------------------------------------------------------------------
    /*static*/ LayoutDescription* LayoutDescription::
      handle_unpack_layout_description(Deserializer &derez,
                                 AddressSpaceID source, RegionNode *region_node)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      bool has_local;
      derez.deserialize(has_local);
      FieldSpaceNode *field_space_node = region_node->column_source;
      LayoutDescription *result = NULL;
      DistributedID constraint_did;
      derez.deserialize(constraint_did);
#ifdef DEBUG_HIGH_LEVEL
      LayoutConstraints *constraints = dynamic_cast<LayoutConstraints*>(
        region_node->context->runtime->find_distributed_collectable(
                                                            constraint_did));
#else
      LayoutConstraints *constraints = static_cast<LayoutConstraints*>(
        region_node->context->runtime->find_distributed_collectable(
                                                            constraint_did));
#endif
      FieldMask mask;
      derez.deserialize(mask);
      size_t blocking_factor;
      derez.deserialize(blocking_factor);
      if (has_local)
      {
        // If we have a local layout, then we should be able to find it
        result = field_space_node->find_layout_description(mask, constraints);
      }
      else
      {
        const std::vector<FieldID> &field_set = 
          constraints->field_constraint.get_field_set();
        std::vector<std::pair<FieldID,size_t> > field_sizes(field_set.size());
        std::vector<unsigned> mask_index_map(field_set.size());
        std::vector<CustomSerdezID> serdez(field_set.size());
        mask.clear();
        field_space_node->compute_create_offsets(field_set, field_sizes,
                                        mask_index_map, serdez, mask);
        result = field_space_node->create_layout_description(mask, constraints,
                                       mask_index_map, serdez, field_sizes);
      }
#ifdef DEBUG_HIGH_LEVEL
      assert(result != NULL);
#endif
      // Record that the sender already has this layout
      // Only do this after we've registered the instance
      result->update_known_nodes(source);
      return result;
    }

    /////////////////////////////////////////////////////////////
    // PhysicalManager 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    PhysicalManager::PhysicalManager(RegionTreeForest *ctx,
                                     MemoryManager *memory, 
                                     LayoutDescription *desc,
                                     const PointerConstraint &constraint,
                                     DistributedID did,
                                     AddressSpaceID owner_space,
                                     AddressSpaceID local_space,
                                     RegionNode *node,
                                     PhysicalInstance inst, const Domain &d, 
                                     bool own, bool register_now)
      : DistributedCollectable(ctx->runtime, did, owner_space, 
                               local_space, register_now), 
        context(ctx), memory_manager(memory), region_node(node), layout(desc),
        instance(inst), instance_domain(d), 
        own_domain(own), pointer_constraint(constraint)
    //--------------------------------------------------------------------------
    {
      if (register_now)
        region_node->register_physical_manager(this);
      // If we are not the owner, add a resource reference
      if (!is_owner())
      {
        // Register it with the memory manager, the memory manager
        // on the owner node will handle this
        memory_manager->register_remote_instance(this);
        add_base_resource_ref(REMOTE_DID_REF);
      }
    }

    //--------------------------------------------------------------------------
    PhysicalManager::~PhysicalManager(void)
    //--------------------------------------------------------------------------
    {
      // Only do the unregistration if we were successfully registered
      if (registered_with_runtime)
        region_node->unregister_physical_manager(this);
      // If we're the owner remove our resource references
      if (is_owner())
      {
        UpdateReferenceFunctor<RESOURCE_REF_KIND,false/*add*/> functor(this);
        map_over_remote_instances(functor);
      }
      else
        memory_manager->unregister_remote_instance(this);
      if (is_owner() && instance.exists())
      {
        log_leak.warning("Leaking physical instance " IDFMT " in memory"
                               IDFMT "", instance.id, get_memory().id);
      }
      // If we own our domain, then we need to delete it now
      if (own_domain)
      {
        Realm::IndexSpace is = instance_domain.get_index_space();
        is.destroy();
      }
    }

    //--------------------------------------------------------------------------
    void PhysicalManager::notify_active(void)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      if (is_owner())
        assert(instance.exists());
#endif
      // Will be null for virtual managers
      if (memory_manager != NULL)
        memory_manager->activate_instance(this);
      // If we are not the owner, send a reference
      if (!is_owner())
        send_remote_gc_update(owner_space, 1/*count*/, true/*add*/);
    }

    //--------------------------------------------------------------------------
    void PhysicalManager::notify_inactive(void)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      if (is_owner())
        assert(instance.exists());
#endif
      // Will be null for virtual managers
      if (memory_manager != NULL)
        memory_manager->deactivate_instance(this);
      if (!is_owner())
        send_remote_gc_update(owner_space, 1/*count*/, false/*add*/);
    }

    //--------------------------------------------------------------------------
    void PhysicalManager::notify_valid(void)
    //--------------------------------------------------------------------------
    {
      // No need to do anything
#ifdef DEBUG_HIGH_LEVEL
      if (is_owner())
        assert(instance.exists());
#endif
      // Will be null for virtual managers
      if (memory_manager != NULL)
        memory_manager->validate_instance(this);
      // If we are not the owner, send a reference
      if (!is_owner())
        send_remote_valid_update(owner_space, 1/*count*/, true/*add*/);
    }

    //--------------------------------------------------------------------------
    void PhysicalManager::notify_invalid(void)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      if (is_owner())
        assert(instance.exists());
#endif
      // Will be null for virtual managers
      if (memory_manager != NULL)
        memory_manager->invalidate_instance(this);
      if (!is_owner())
        send_remote_valid_update(owner_space, 1/*count*/, false/*add*/);
    }

    //--------------------------------------------------------------------------
    void PhysicalManager::register_logical_top_view(UniqueID context_uid,
                                                    InstanceView *top_view)
    //--------------------------------------------------------------------------
    {
      UserEvent to_trigger = UserEvent::NO_USER_EVENT;
      {
        // Co-opt the gc lock for synchronization
        AutoLock gc(gc_lock);
#ifdef DEBUG_HIGH_LEVEL
        assert(top_views.find(context_uid) == top_views.end());
        assert(top_reverse.find(top_view) == top_reverse.end());
#endif
        top_views[context_uid] = top_view;
        top_reverse[top_view] = context_uid;
        // See if we have any events to trigger
        std::map<UniqueID,UserEvent>::const_iterator finder = 
          pending_views.find(context_uid);
        if (finder != pending_views.end())
          to_trigger = finder->second;
        else
          return;
      }
      to_trigger.trigger();
    }

    //--------------------------------------------------------------------------
    void PhysicalManager::unregister_logical_top_view(InstanceView *top_view)
    //--------------------------------------------------------------------------
    {
      // Co-opt the gc lock for synchronization
      AutoLock gc(gc_lock);
      std::map<InstanceView*,UniqueID>::iterator finder = 
        top_reverse.find(top_view);
#ifdef DEBUG_HIGH_LEVEL
      assert(finder != top_reverse.end());
      assert(top_views.find(finder->second) != top_views.end());
#endif
      top_views.erase(finder->second);
      top_reverse.erase(finder);
    }

    //--------------------------------------------------------------------------
    UniqueID PhysicalManager::find_context_uid(InstanceView *top_view) const
    //--------------------------------------------------------------------------
    {
      // Co-opt the gc lock for synchronization
      AutoLock gc(gc_lock,1,false/*exclusive*/);
      std::map<InstanceView*,UniqueID>::const_iterator finder = 
        top_reverse.find(top_view);
#ifdef DEBUG_HIGH_LEVEL
      assert(finder != top_reverse.end());
#endif
      return finder->second;
    }

    //--------------------------------------------------------------------------
    InstanceView* PhysicalManager::find_or_create_logical_top_view(
                                                           UniqueID context_uid)
    //--------------------------------------------------------------------------
    {
      // See if we've already got it
      bool need_creation = false;
      Event wait_on = Event::NO_EVENT;
      {
        AutoLock gc(gc_lock);
        std::map<UniqueID,InstanceView*>::const_iterator finder = 
          top_views.find(context_uid);
        if (finder != top_views.end())
          return finder->second;
        // If we didn't find it, see if someone else is already making it
        std::map<UniqueID,UserEvent>::const_iterator pending_finder = 
          pending_views.find(context_uid);
        if (pending_finder == pending_views.end())
        {
          need_creation = true;
          UserEvent make_event = UserEvent::create_user_event();
          pending_views[context_uid] = make_event;
          wait_on = make_event;
        }
        else
          wait_on = pending_finder->second;
      }
      if (need_creation)
      {
        // See if we are local or remote
        if (!is_owner())
        {
          Serializer rez;
          {
            RezCheck z(rez);
            rez.serialize(did);
            rez.serialize(context_uid);
          }
          runtime->send_create_top_view_request(owner_space, rez);
        }
        else // we're the owner so make the view
          return create_logical_top_view(context_uid);
      }
      // Wait for the results
      wait_on.wait();
      AutoLock gc(gc_lock,1,false/*exclusive*/);
#ifdef DEBUG_HIGH_LEVEL
      // It better be there when this gets triggered
      assert(top_views.find(context_uid) != top_views.end());
#endif
      return top_views[context_uid];
    }

    //--------------------------------------------------------------------------
    bool PhysicalManager::meets_region_tree(
                                const std::vector<LogicalRegion> &regions) const
    //--------------------------------------------------------------------------
    {
      for (std::vector<LogicalRegion>::const_iterator it = 
            regions.begin(); it != regions.end(); it++)
      {
        // Check to see if the region tree IDs are the same
        if (it->get_tree_id() != region_node->handle.get_tree_id())
          return false;
      }
      return true;
    }

    //--------------------------------------------------------------------------
    bool PhysicalManager::meets_regions(
                                const std::vector<LogicalRegion> &regions) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(region_node != NULL); // only happens with VirtualManager
#endif
      RegionTreeID tree_id = region_node->handle.get_tree_id();
      for (std::vector<LogicalRegion>::const_iterator it = 
            regions.begin(); it != regions.end(); it++)
      {
        // If they are not the same tree ID that is really bad
        if (tree_id != it->get_tree_id())
          return false;
        RegionNode *handle_node = context->get_node(*it);
        // Now check to see if our instance domain dominates the region
        IndexSpaceNode *index_node = handle_node->row_source; 
        std::vector<Domain> to_check;
        index_node->get_domains_blocking(to_check);
        switch (instance_domain.get_dim())
        {
          case 0:
            {
              // For now just check to make sure we have space
              const size_t space_size = instance_domain.get_index_space().
                                          get_valid_mask().get_num_elmts();
              for (unsigned idx = 0; idx < to_check.size(); idx++)
              {
                const size_t other_size = to_check[idx].get_index_space().
                                          get_valid_mask().get_num_elmts();
                if (space_size < other_size)
                  return false;
              }
              break;
            }
          case 1:
            {
              LegionRuntime::Arrays::Rect<1> our_rect = 
                instance_domain.get_rect<1>();
              for (unsigned idx = 0; idx < to_check.size(); idx++)
              {
                LegionRuntime::Arrays::Rect<1> other_rect = 
                  to_check[idx].get_rect<1>();
                if (!our_rect.dominates(other_rect))
                  return false;
              }
              break;
            }
          case 2:
            {
              LegionRuntime::Arrays::Rect<2> our_rect = 
                instance_domain.get_rect<2>();
              for (unsigned idx = 0; idx < to_check.size(); idx++)
              {
                LegionRuntime::Arrays::Rect<2> other_rect = 
                  to_check[idx].get_rect<2>();
                if (!our_rect.dominates(other_rect))
                  return false;
              }
              break;
            }
          case 3:
            {
              LegionRuntime::Arrays::Rect<3> our_rect = 
                instance_domain.get_rect<3>();
              for (unsigned idx = 0; idx < to_check.size(); idx++)
              {
                LegionRuntime::Arrays::Rect<3> other_rect = 
                  to_check[idx].get_rect<3>();
                if (!our_rect.dominates(other_rect))
                  return false;
              }
              break;
            }
          default:
            assert(false); // unhandled number of dimensions
        }
      }
      return true;
    }

    //--------------------------------------------------------------------------
    bool PhysicalManager::entails(LayoutConstraints *constraints) const
    //--------------------------------------------------------------------------
    {
      // Always test the pointer constraint locally
      if (!pointer_constraint.entails(constraints->pointer_constraint))
        return false;
      return layout->constraints->entails_without_pointer(constraints);
    }

    //--------------------------------------------------------------------------
    bool PhysicalManager::entails(const LayoutConstraintSet &constraints) const
    //--------------------------------------------------------------------------
    {
      // Always test the pointer constraint locally
      if (!pointer_constraint.entails(constraints.pointer_constraint))
        return false;
      return layout->constraints->entails_without_pointer(constraints);
    }

    //--------------------------------------------------------------------------
    bool PhysicalManager::conflicts(LayoutConstraints *constraints) const
    //--------------------------------------------------------------------------
    {
      // Always test the pointer constraint locally
      if (pointer_constraint.conflicts(constraints->pointer_constraint))
        return true;
      // We know our layouts don't have a pointer constraint so nothing special
      return layout->constraints->conflicts(constraints);
    }

    //--------------------------------------------------------------------------
    bool PhysicalManager::conflicts(const LayoutConstraintSet &constraints)const
    //--------------------------------------------------------------------------
    {
      // Always test the pointer constraint locally
      if (pointer_constraint.conflicts(constraints.pointer_constraint))
        return true;
      // We know our layouts don't have a pointer constraint so nothing special
      return layout->constraints->conflicts(constraints);
    }

    //--------------------------------------------------------------------------
    void PhysicalManager::perform_deletion(Event deferred_event) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(is_owner());
#endif
      log_garbage.info("Deleting physical instance " IDFMT " in memory " 
                       IDFMT "", instance.id, memory_manager->memory.id);
#ifndef DISABLE_GC
      std::vector<PhysicalInstance::DestroyedField> serdez_fields;
      layout->compute_destroyed_fields(serdez_fields); 
      if (!serdez_fields.empty())
        instance.destroy(serdez_fields, deferred_event);
      else
        instance.destroy(deferred_event);
#endif
    }

    //--------------------------------------------------------------------------
    void PhysicalManager::set_garbage_collection_priority(MapperID mapper_id,
                                            Processor proc, GCPriority priority)
    //--------------------------------------------------------------------------
    {
      memory_manager->set_garbage_collection_priority(this, mapper_id,
                                                      proc, priority);
    }

    //--------------------------------------------------------------------------
    /*static*/ void PhysicalManager::delete_physical_manager(
                                                       PhysicalManager *manager)
    //--------------------------------------------------------------------------
    {
      if (manager->is_reduction_manager())
      {
        ReductionManager *reduc_manager = manager->as_reduction_manager();
        if (reduc_manager->is_list_manager())
          legion_delete(reduc_manager->as_list_manager());
        else
          legion_delete(reduc_manager->as_fold_manager());
      }
      else
        legion_delete(manager->as_instance_manager());
    }

    //--------------------------------------------------------------------------
    /*static*/ void PhysicalManager::handle_create_top_view_request(
                   Runtime *runtime, AddressSpaceID source, Deserializer &derez)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID did;
      derez.deserialize(did);
      UniqueID context_uid;
      derez.deserialize(context_uid);
#ifdef DEBUG_HIGH_LEVEL
      PhysicalManager *manager = dynamic_cast<PhysicalManager*>(
          runtime->find_distributed_collectable(did));
      assert(manager != NULL);
#else
      PhysicalManager *manager = static_cast<PhysicalManager*>(
          runtime->find_distributed_collectable(did));
#endif
      InstanceView *target_view = 
        manager->find_or_create_logical_top_view(context_uid);
      target_view->send_view_base(source);
    }

    /////////////////////////////////////////////////////////////
    // InstanceManager 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    InstanceManager::InstanceManager(RegionTreeForest *ctx, DistributedID did,
                                     AddressSpaceID owner_space, 
                                     AddressSpaceID local_space,
                                     MemoryManager *mem, PhysicalInstance inst,
                                     const Domain &instance_domain, bool own,
                                     RegionNode *node, LayoutDescription *desc, 
                                     const PointerConstraint &constraint,
                                     Event u_event, bool reg_now, 
                                     InstanceFlag flags)
      : PhysicalManager(ctx, mem, desc, constraint, did, owner_space, 
                        local_space, node, inst, instance_domain, own, reg_now),
        use_event(u_event), instance_flags(flags)
    //--------------------------------------------------------------------------
    {
      // Add a reference to the layout
      layout->add_reference();
#ifdef LEGION_GC
      log_garbage.info("GC Instance Manager %ld " IDFMT " " IDFMT " ",
                        did, inst.id, mem.id);
#endif
      if (is_owner() && Runtime::legion_spy_enabled)
      {
        LegionSpy::log_physical_instance(inst.id, mem->memory.id, 0);
        LegionSpy::log_physical_instance_region(inst.id, region_node->handle);
        layout->log_instance_layout(inst);
      }
    }

    //--------------------------------------------------------------------------
    InstanceManager::InstanceManager(const InstanceManager &rhs)
      : PhysicalManager(NULL, NULL, NULL, rhs.pointer_constraint, 0, 0, 0, NULL,
                        PhysicalInstance::NO_INST, 
                        Domain::NO_DOMAIN, false, false),
        use_event(Event::NO_EVENT)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    InstanceManager::~InstanceManager(void)
    //--------------------------------------------------------------------------
    {
      if (layout->remove_reference())
        delete layout;
    }

    //--------------------------------------------------------------------------
    InstanceManager& InstanceManager::operator=(const InstanceManager &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    LegionRuntime::Accessor::RegionAccessor<
      LegionRuntime::Accessor::AccessorType::Generic>
        InstanceManager::get_accessor(void) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(instance.exists());
#endif
      return instance.get_accessor();
    }

    //--------------------------------------------------------------------------
    LegionRuntime::Accessor::RegionAccessor<
      LegionRuntime::Accessor::AccessorType::Generic>
        InstanceManager::get_field_accessor(FieldID fid) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(instance.exists());
      assert(layout != NULL);
#endif
      const Domain::CopySrcDstField &info = layout->find_field_info(fid);
      LegionRuntime::Accessor::RegionAccessor<
        LegionRuntime::Accessor::AccessorType::Generic> temp = 
                                                    instance.get_accessor();
      return temp.get_untyped_field_accessor(info.offset, info.size);
    }

    //--------------------------------------------------------------------------
    bool InstanceManager::is_reduction_manager(void) const
    //--------------------------------------------------------------------------
    {
      return false;
    }

    //--------------------------------------------------------------------------
    bool InstanceManager::is_instance_manager(void) const
    //--------------------------------------------------------------------------
    {
      return true;
    }

    //--------------------------------------------------------------------------
    bool InstanceManager::is_virtual_manager(void) const
    //--------------------------------------------------------------------------
    {
      return false;
    }

#ifdef DEBUG_HIGH_LEVEL
    //--------------------------------------------------------------------------
    InstanceManager* InstanceManager::as_instance_manager(void) const
    //--------------------------------------------------------------------------
    {
      return const_cast<InstanceManager*>(this);
    }

    //--------------------------------------------------------------------------
    ReductionManager* InstanceManager::as_reduction_manager(void) const
    //--------------------------------------------------------------------------
    {
      return NULL;
    }

    //--------------------------------------------------------------------------
    VirtualManager* InstanceManager::as_virtual_manager(void) const
    //--------------------------------------------------------------------------
    {
      return NULL;
    }
#endif

    //--------------------------------------------------------------------------
    size_t InstanceManager::get_instance_size(void) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(layout != NULL);
#endif
      size_t field_sizes = layout->get_total_field_size();
      size_t volume = 
        region_node->row_source->get_domain_blocking().get_volume();
      return (field_sizes * volume);
    }

    //--------------------------------------------------------------------------
    InstanceView* InstanceManager::create_logical_top_view(UniqueID ctx_uid)
    //--------------------------------------------------------------------------
    {
      DistributedID view_did = 
        context->runtime->get_available_distributed_id(false);
      MaterializedView *result = legion_new<MaterializedView>(context, view_did,
                                                context->runtime->address_space,
                                                context->runtime->address_space,
                                                region_node, this,
                                            ((MaterializedView*)NULL/*parent*/),
                                                true/*register now*/,
                                                ctx_uid);
      return result;
    }

    //--------------------------------------------------------------------------
    void InstanceManager::compute_copy_offsets(const FieldMask &copy_mask,
                                  std::vector<Domain::CopySrcDstField> &fields)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(layout != NULL);
#endif
      // Pass in our physical instance so the layout knows how to specialize
      layout->compute_copy_offsets(copy_mask, instance, fields);
    }

    //--------------------------------------------------------------------------
    void InstanceManager::compute_copy_offsets(FieldID fid,
                                  std::vector<Domain::CopySrcDstField> &fields)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(layout != NULL);
#endif
      // Pass in our physical instance so the layout knows how to specialize
      layout->compute_copy_offsets(fid, instance, fields);
    }

    //--------------------------------------------------------------------------
    void InstanceManager::compute_copy_offsets(
                                  const std::vector<FieldID> &copy_fields,
                                  std::vector<Domain::CopySrcDstField> &fields)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(layout != NULL);
#endif
      // Pass in our physical instance so the layout knows how to specialize
      layout->compute_copy_offsets(copy_fields, instance, fields);
    }

    //--------------------------------------------------------------------------
    void InstanceManager::set_descriptor(FieldDataDescriptor &desc,
                                         unsigned fid_idx) const
    //--------------------------------------------------------------------------
    {
      // Fill in the information about our instance
      desc.inst = instance;
      // Ask the layout to fill in the information about field offset and size
      layout->set_descriptor(desc, fid_idx);
    }

    //--------------------------------------------------------------------------
    DistributedID InstanceManager::send_manager(AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      if (!has_remote_instance(target))
      {
        // No need to take the lock, duplicate sends are alright
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(did);
          rez.serialize(owner_space);
          rez.serialize(memory_manager->memory);
          rez.serialize(instance);
          rez.serialize(instance_domain);
          rez.serialize(region_node->handle);
          rez.serialize(use_event);
          rez.serialize(instance_flags);
          layout->pack_layout_description(rez, target);
          pointer_constraint.serialize(rez);
        }
        context->runtime->send_instance_manager(target, rez);
        update_remote_instances(target);
        // Finally we can update our known nodes
        // It's only safe to do this after the message
        // has been sent
        layout->update_known_nodes(target);
      }
      return did;
    }

    //--------------------------------------------------------------------------
    /*static*/ void InstanceManager::handle_send_manager(Runtime *runtime, 
                                     AddressSpaceID source, Deserializer &derez)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID did;
      derez.deserialize(did);
      AddressSpaceID owner_space;
      derez.deserialize(owner_space);
      Memory mem;
      derez.deserialize(mem);
      PhysicalInstance inst;
      derez.deserialize(inst);
      Domain inst_domain;
      derez.deserialize(inst_domain);
      LogicalRegion handle;
      derez.deserialize(handle);
      Event use_event;
      derez.deserialize(use_event);
      InstanceFlag flags;
      derez.deserialize(flags);
      RegionNode *target_node = runtime->forest->get_node(handle);
      LayoutDescription *layout = 
        LayoutDescription::handle_unpack_layout_description(derez, source, 
                                                            target_node);
      PointerConstraint pointer_constraint;
      pointer_constraint.deserialize(derez);
      MemoryManager *memory = runtime->find_memory_manager(mem);
      InstanceManager *inst_manager = legion_new<InstanceManager>(
                                        runtime->forest, did, owner_space,
                                        runtime->address_space, memory, inst,
                                        inst_domain, false/*owns*/,
                                        target_node, layout, 
                                        pointer_constraint, use_event,
                                        false/*reg now*/, flags);
      if (!target_node->register_physical_manager(inst_manager))
      {
        if (inst_manager->remove_base_resource_ref(REMOTE_DID_REF))
          legion_delete(inst_manager);
      }
      else
      {
        inst_manager->register_with_runtime();
        inst_manager->update_remote_instances(source);
      }
    }

    //--------------------------------------------------------------------------
    bool InstanceManager::is_attached_file(void) const
    //--------------------------------------------------------------------------
    {
      return (instance_flags & ATTACH_FILE_FLAG);
    }

    /////////////////////////////////////////////////////////////
    // ReductionManager 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    ReductionManager::ReductionManager(RegionTreeForest *ctx, DistributedID did,
                                       FieldID f, AddressSpaceID owner_space, 
                                       AddressSpaceID local_space,
                                       MemoryManager *mem,PhysicalInstance inst,
                                       LayoutDescription *desc, 
                                       const PointerConstraint &constraint,
                                       const Domain &inst_domain, bool own_dom,
                                       RegionNode *node, ReductionOpID red, 
                                       const ReductionOp *o, bool reg_now)
      : PhysicalManager(ctx, mem, desc, constraint, did, owner_space, 
                        local_space, node, inst, inst_domain, own_dom, reg_now),
        op(o), redop(red), logical_field(f)
    //--------------------------------------------------------------------------
    { 
      if (is_owner() && Runtime::legion_spy_enabled)
      {
        LegionSpy::log_physical_instance(inst.id, mem->memory.id, redop);
        LegionSpy::log_physical_instance_region(inst.id, region_node->handle);
        layout->log_instance_layout(inst);
      }
    }

    //--------------------------------------------------------------------------
    ReductionManager::~ReductionManager(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    bool ReductionManager::is_reduction_manager(void) const
    //--------------------------------------------------------------------------
    {
      return true;
    }

    //--------------------------------------------------------------------------
    bool ReductionManager::is_instance_manager(void) const
    //--------------------------------------------------------------------------
    {
      return false;
    }

    //--------------------------------------------------------------------------
    bool ReductionManager::is_virtual_manager(void) const
    //--------------------------------------------------------------------------
    {
      return false;
    }

#ifdef DEBUG_HIGH_LEVEL
    //--------------------------------------------------------------------------
    InstanceManager* ReductionManager::as_instance_manager(void) const
    //--------------------------------------------------------------------------
    {
      return NULL;
    }

    //--------------------------------------------------------------------------
    ReductionManager* ReductionManager::as_reduction_manager(void) const
    //--------------------------------------------------------------------------
    {
      return const_cast<ReductionManager*>(this);
    }

    //--------------------------------------------------------------------------
    VirtualManager* ReductionManager::as_virtual_manager(void) const
    //--------------------------------------------------------------------------
    {
      return NULL;
    }
#endif

    //--------------------------------------------------------------------------
    bool ReductionManager::has_field(FieldID fid) const
    //--------------------------------------------------------------------------
    {
      return (logical_field == fid); 
    }

    //--------------------------------------------------------------------------
    void ReductionManager::has_fields(std::map<FieldID,bool> &fields) const
    //--------------------------------------------------------------------------
    {
      for (std::map<FieldID,bool>::iterator it = fields.begin();
            it != fields.end(); it++)
      {
        if (it->first == logical_field)
          it->second = true;
        else
          it->second = false;
      }
    }

    //--------------------------------------------------------------------------
    void ReductionManager::remove_space_fields(std::set<FieldID> &fields) const
    //--------------------------------------------------------------------------
    {
      fields.erase(logical_field);
    }

    //--------------------------------------------------------------------------
    DistributedID ReductionManager::send_manager(AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      if (!has_remote_instance(target))
      {
        // NO need to take the lock, duplicate sends are alright
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(did);
          rez.serialize(owner_space);
          rez.serialize(memory_manager->memory);
          rez.serialize(instance);
          rez.serialize(instance_domain);
          rez.serialize(redop);
          rez.serialize(logical_field);
          rez.serialize(region_node->handle);
          rez.serialize<bool>(is_foldable());
          rez.serialize(get_pointer_space());
          rez.serialize(get_use_event());
          layout->pack_layout_description(rez, target);
          pointer_constraint.serialize(rez);
        }
        // Now send the message
        context->runtime->send_reduction_manager(target, rez);
        update_remote_instances(target);
      }
      return did;
    }

    //--------------------------------------------------------------------------
    /*static*/ void ReductionManager::handle_send_manager(Runtime *runtime, 
                                     AddressSpaceID source, Deserializer &derez)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID did;
      derez.deserialize(did);
      AddressSpaceID owner_space;
      derez.deserialize(owner_space);
      Memory mem;
      derez.deserialize(mem);
      PhysicalInstance inst;
      derez.deserialize(inst);
      Domain inst_dom;
      derez.deserialize(inst_dom);
      ReductionOpID redop;
      derez.deserialize(redop);
      FieldID logical_field;
      derez.deserialize(logical_field);
      LogicalRegion handle;
      derez.deserialize(handle);
      bool foldable;
      derez.deserialize(foldable);
      Domain ptr_space;
      derez.deserialize(ptr_space);
      Event use_event;
      derez.deserialize(use_event);
      RegionNode *target_node = runtime->forest->get_node(handle);
      LayoutDescription *layout = 
        LayoutDescription::handle_unpack_layout_description(derez, source, 
                                                            target_node);
      PointerConstraint pointer_constraint;
      pointer_constraint.deserialize(derez);
      MemoryManager *memory = runtime->find_memory_manager(mem);
      const ReductionOp *op = Runtime::get_reduction_op(redop);
      if (foldable)
      {
        FoldReductionManager *manager = 
                        legion_new<FoldReductionManager>(
                                            runtime->forest, did, logical_field,
                                            owner_space, runtime->address_space,
                                            memory, inst, layout,
                                            pointer_constraint, inst_dom,
                                            false/*own*/, target_node, redop,op,
                                            use_event, false/*register now*/);
        if (!target_node->register_physical_manager(manager))
          legion_delete(manager);
        else
        {
          manager->register_with_runtime();
          manager->update_remote_instances(source);
        }
      }
      else
      {
        ListReductionManager *manager = 
                        legion_new<ListReductionManager>(
                                            runtime->forest, did, logical_field,
                                            owner_space, runtime->address_space,
                                            memory, inst, layout, 
                                            pointer_constraint, inst_dom, 
                                            false/*own*/, target_node, redop,op,
                                            ptr_space, false/*register now*/);
        if (!target_node->register_physical_manager(manager))
          legion_delete(manager);
        else
        {
          manager->register_with_runtime();
          manager->update_remote_instances(source);
        }
      }
    }

    //--------------------------------------------------------------------------
    InstanceView* ReductionManager::create_logical_top_view(
                                                           UniqueID context_uid)
    //--------------------------------------------------------------------------
    {
      DistributedID view_did = 
        context->runtime->get_available_distributed_id(false);
      ReductionView *result = legion_new<ReductionView>(context, view_did,
                                                context->runtime->address_space,
                                                context->runtime->address_space,
                                                region_node, this, 
                                                true/*reg*/, context_uid);
      return result;
    }

    /////////////////////////////////////////////////////////////
    // ListReductionManager 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    ListReductionManager::ListReductionManager(RegionTreeForest *ctx, 
                                               DistributedID did,
                                               FieldID f,
                                               AddressSpaceID owner_space, 
                                               AddressSpaceID local_space,
                                               MemoryManager *mem,
                                               PhysicalInstance inst, 
                                               LayoutDescription *desc,
                                               const PointerConstraint &cons,
                                               const Domain &d, bool own_dom,
                                               RegionNode *node,
                                               ReductionOpID red,
                                               const ReductionOp *o, 
                                               Domain dom, bool reg_now)
      : ReductionManager(ctx, did, f, owner_space, local_space, mem, 
                         inst, desc, cons, d, own_dom, node, red, o, reg_now),
        ptr_space(dom)
    //--------------------------------------------------------------------------
    {
#ifdef LEGION_GC
      log_garbage.info("GC List Reduction Manager %ld " IDFMT " " IDFMT " ",
                        did, inst.id, mem.id);
#endif
    }

    //--------------------------------------------------------------------------
    ListReductionManager::ListReductionManager(const ListReductionManager &rhs)
      : ReductionManager(NULL, 0, 0, 0, 0, NULL,
                         PhysicalInstance::NO_INST, NULL,rhs.pointer_constraint,
                         Domain::NO_DOMAIN, false, NULL, 0, NULL, false),
        ptr_space(Domain::NO_DOMAIN)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    ListReductionManager::~ListReductionManager(void)
    //--------------------------------------------------------------------------
    {
      // Free up our pointer space
      ptr_space.get_index_space().destroy();
    }

    //--------------------------------------------------------------------------
    ListReductionManager& ListReductionManager::operator=(
                                                const ListReductionManager &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    LegionRuntime::Accessor::RegionAccessor<
      LegionRuntime::Accessor::AccessorType::Generic>
        ListReductionManager::get_accessor(void) const
    //--------------------------------------------------------------------------
    {
      // TODO: Implement this 
      assert(false);
      return instance.get_accessor();
    }

    //--------------------------------------------------------------------------
    LegionRuntime::Accessor::RegionAccessor<
      LegionRuntime::Accessor::AccessorType::Generic>
        ListReductionManager::get_field_accessor(FieldID fid) const
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return instance.get_accessor();
    }

    //--------------------------------------------------------------------------
    size_t ListReductionManager::get_instance_size(void) const
    //--------------------------------------------------------------------------
    {
      size_t result = op->sizeof_rhs;
      if (ptr_space.get_dim() == 0)
      {
        const Realm::ElementMask &mask = 
          ptr_space.get_index_space().get_valid_mask();
        result *= mask.get_num_elmts();
      }
      else
        result *= ptr_space.get_volume();
      return result;
    }
    
    //--------------------------------------------------------------------------
    bool ListReductionManager::is_foldable(void) const
    //--------------------------------------------------------------------------
    {
      return false;
    }

    //--------------------------------------------------------------------------
    void ListReductionManager::find_field_offsets(const FieldMask &reduce_mask,
                                  std::vector<Domain::CopySrcDstField> &fields)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(instance.exists());
#endif
      // Assume that it's all the fields for right now
      // but offset by the pointer size
      fields.push_back(
          Domain::CopySrcDstField(instance, sizeof(ptr_t), op->sizeof_rhs));
    }

    //--------------------------------------------------------------------------
    Event ListReductionManager::issue_reduction(Operation *op,
        const std::vector<Domain::CopySrcDstField> &src_fields,
        const std::vector<Domain::CopySrcDstField> &dst_fields,
        Domain space, Event precondition, bool reduction_fold, bool precise)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(instance.exists());
#endif
      if (precise)
      {
        Domain::CopySrcDstField idx_field(instance, 0/*offset*/, sizeof(ptr_t));
        return context->issue_indirect_copy(space, op, idx_field, redop, 
                                            reduction_fold, src_fields, 
                                            dst_fields, precondition);
      }
      else
      {
        // TODO: teach the low-level runtime how to issue
        // partial reduction copies from a given space
        assert(false);
        return Event::NO_EVENT;
      }
    }

    //--------------------------------------------------------------------------
    Domain ListReductionManager::get_pointer_space(void) const
    //--------------------------------------------------------------------------
    {
      return ptr_space;
    }

    //--------------------------------------------------------------------------
    bool ListReductionManager::is_list_manager(void) const
    //--------------------------------------------------------------------------
    {
      return true;
    }

#ifdef DEBUG_HIGH_LEVEL
    //--------------------------------------------------------------------------
    ListReductionManager* ListReductionManager::as_list_manager(void) const
    //--------------------------------------------------------------------------
    {
      return const_cast<ListReductionManager*>(this);
    }

    //--------------------------------------------------------------------------
    FoldReductionManager* ListReductionManager::as_fold_manager(void) const
    //--------------------------------------------------------------------------
    {
      return NULL;
    }
#endif

    //--------------------------------------------------------------------------
    Event ListReductionManager::get_use_event(void) const
    //--------------------------------------------------------------------------
    {
      return Event::NO_EVENT;
    }

    /////////////////////////////////////////////////////////////
    // FoldReductionManager 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    FoldReductionManager::FoldReductionManager(RegionTreeForest *ctx, 
                                               DistributedID did,
                                               FieldID f,
                                               AddressSpaceID owner_space, 
                                               AddressSpaceID local_space,
                                               MemoryManager *mem,
                                               PhysicalInstance inst, 
                                               LayoutDescription *desc,
                                               const PointerConstraint &cons,
                                               const Domain &d, bool own_dom,
                                               RegionNode *node,
                                               ReductionOpID red,
                                               const ReductionOp *o,
                                               Event u_event,
                                               bool register_now)
      : ReductionManager(ctx, did, f, owner_space, local_space, mem, inst, 
                         desc, cons, d, own_dom, node, red, o,register_now),
        use_event(u_event)
    //--------------------------------------------------------------------------
    {
#ifdef LEGION_GC
      log_garbage.info("GC Fold Reduction Manager %ld " IDFMT " " IDFMT " ",
                        did, inst.id, mem.id);
#endif
    }

    //--------------------------------------------------------------------------
    FoldReductionManager::FoldReductionManager(const FoldReductionManager &rhs)
      : ReductionManager(NULL, 0, 0, 0, 0, NULL,
                         PhysicalInstance::NO_INST, NULL,rhs.pointer_constraint,
                         Domain::NO_DOMAIN, false, NULL, 0, NULL, false),
        use_event(Event::NO_EVENT)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    FoldReductionManager::~FoldReductionManager(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    FoldReductionManager& FoldReductionManager::operator=(
                                                const FoldReductionManager &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    LegionRuntime::Accessor::RegionAccessor<
      LegionRuntime::Accessor::AccessorType::Generic>
        FoldReductionManager::get_accessor(void) const
    //--------------------------------------------------------------------------
    {
      return instance.get_accessor();
    }

    //--------------------------------------------------------------------------
    LegionRuntime::Accessor::RegionAccessor<
      LegionRuntime::Accessor::AccessorType::Generic>
        FoldReductionManager::get_field_accessor(FieldID fid) const
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return instance.get_accessor();
    }

    //--------------------------------------------------------------------------
    size_t FoldReductionManager::get_instance_size(void) const
    //--------------------------------------------------------------------------
    {
      size_t result = op->sizeof_rhs;
      const Domain &d = region_node->row_source->get_domain_blocking();
      if (d.get_dim() == 0)
      {
        const Realm::ElementMask &mask = 
          d.get_index_space().get_valid_mask();
        result *= mask.get_num_elmts();
      }
      else
        result *= d.get_volume();
      return result;
    }
    
    //--------------------------------------------------------------------------
    bool FoldReductionManager::is_foldable(void) const
    //--------------------------------------------------------------------------
    {
      return true;
    }

    //--------------------------------------------------------------------------
    void FoldReductionManager::find_field_offsets(const FieldMask &reduce_mask,
                                  std::vector<Domain::CopySrcDstField> &fields)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(instance.exists());
#endif
      // Assume that its all the fields for now
      // until we find a different way to do reductions on a subset of fields
      fields.push_back(
          Domain::CopySrcDstField(instance, 0/*offset*/, op->sizeof_rhs));
    }

    //--------------------------------------------------------------------------
    Event FoldReductionManager::issue_reduction(Operation *op,
        const std::vector<Domain::CopySrcDstField> &src_fields,
        const std::vector<Domain::CopySrcDstField> &dst_fields,
        Domain space, Event precondition, bool reduction_fold, bool precise)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_HIGH_LEVEL
      assert(instance.exists());
#endif
      // Doesn't matter if this one is precise or not
      return context->issue_reduction_copy(space, op, redop, reduction_fold,
                                         src_fields, dst_fields, precondition);
    }

    //--------------------------------------------------------------------------
    Domain FoldReductionManager::get_pointer_space(void) const
    //--------------------------------------------------------------------------
    {
      return Domain::NO_DOMAIN;
    }

    //--------------------------------------------------------------------------
    bool FoldReductionManager::is_list_manager(void) const
    //--------------------------------------------------------------------------
    {
      return false;
    }

#ifdef DEBUG_HIGH_LEVEL
    //--------------------------------------------------------------------------
    ListReductionManager* FoldReductionManager::as_list_manager(void) const
    //--------------------------------------------------------------------------
    {
      return NULL;
    }

    //--------------------------------------------------------------------------
    FoldReductionManager* FoldReductionManager::as_fold_manager(void) const
    //--------------------------------------------------------------------------
    {
      return const_cast<FoldReductionManager*>(this);
    }
#endif

    //--------------------------------------------------------------------------
    Event FoldReductionManager::get_use_event(void) const
    //--------------------------------------------------------------------------
    {
      return use_event;
    }

    /////////////////////////////////////////////////////////////
    // Virtual Manager 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    VirtualManager::VirtualManager(RegionTreeForest *ctx, 
                                   LayoutDescription *desc,
                                   const PointerConstraint &constraint,
                                   DistributedID did,AddressSpaceID local_space)
      : PhysicalManager(ctx, NULL/*memory*/, desc, constraint, did, local_space,
                        local_space, NULL/*region*/, PhysicalInstance::NO_INST,
                        Domain::NO_DOMAIN, false/*own domain*/,false/*reg now*/)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    VirtualManager::VirtualManager(const VirtualManager &rhs)
      : PhysicalManager(NULL, NULL, NULL, rhs.pointer_constraint, 0, 0, 0,
                        NULL, PhysicalInstance::NO_INST, Domain::NO_DOMAIN,
                        false, false)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    VirtualManager::~VirtualManager(void)
    //--------------------------------------------------------------------------
    {
      // this should never be deleted since it is a singleton
      assert(false);
    }

    //--------------------------------------------------------------------------
    VirtualManager& VirtualManager::operator=(const VirtualManager &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    LegionRuntime::Accessor::RegionAccessor<
        LegionRuntime::Accessor::AccessorType::Generic>
          VirtualManager::get_accessor(void) const
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return PhysicalInstance::NO_INST.get_accessor();
    }

    //--------------------------------------------------------------------------
    LegionRuntime::Accessor::RegionAccessor<
        LegionRuntime::Accessor::AccessorType::Generic>
          VirtualManager::get_field_accessor(FieldID fid) const
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return PhysicalInstance::NO_INST.get_accessor();
    }

    //--------------------------------------------------------------------------
    bool VirtualManager::is_reduction_manager(void) const
    //--------------------------------------------------------------------------
    {
      return false;
    }

    //--------------------------------------------------------------------------
    bool VirtualManager::is_instance_manager(void) const
    //--------------------------------------------------------------------------
    {
      return false;
    }

    //--------------------------------------------------------------------------
    bool VirtualManager::is_virtual_manager(void) const
    //--------------------------------------------------------------------------
    {
      return true;
    }

#ifdef DEBUG_HIGH_LEVEL
    //--------------------------------------------------------------------------
    InstanceManager* VirtualManager::as_instance_manager(void) const
    //--------------------------------------------------------------------------
    {
      return NULL;
    }

    //--------------------------------------------------------------------------
    ReductionManager* VirtualManager::as_reduction_manager(void) const
    //--------------------------------------------------------------------------
    {
      return NULL;
    }

    //--------------------------------------------------------------------------
    VirtualManager* VirtualManager::as_virtual_manager(void) const
    //--------------------------------------------------------------------------
    {
      return const_cast<VirtualManager*>(this);
    }
#endif

    //--------------------------------------------------------------------------
    size_t VirtualManager::get_instance_size(void) const
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return 0;
    }

    //--------------------------------------------------------------------------
    DistributedID VirtualManager::send_manager(AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      // no need to do anything
      return did;
    }

    //--------------------------------------------------------------------------
    bool VirtualManager::has_field(FieldID fid) const
    //--------------------------------------------------------------------------
    {
      // has all fields
      return true;
    }

    //--------------------------------------------------------------------------
    void VirtualManager::has_fields(std::map<FieldID,bool> &fields) const
    //--------------------------------------------------------------------------
    {
      for (std::map<FieldID,bool>::iterator it = fields.begin();
            it != fields.end(); it++)
        it->second = true;
    }

    //--------------------------------------------------------------------------
    void VirtualManager::remove_space_fields(std::set<FieldID> &fields) const
    //--------------------------------------------------------------------------
    {
      fields.clear();
    }

    //--------------------------------------------------------------------------
    InstanceView* VirtualManager::create_logical_top_view(UniqueID context_uid)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return NULL;
    }

    //--------------------------------------------------------------------------
    /*static*/ void VirtualManager::initialize_virtual_instance(Runtime *rt,
                                                              DistributedID did)
    //--------------------------------------------------------------------------
    {
      VirtualManager *&singleton = get_singleton();
      // make a layout constraints
      LayoutConstraintSet constraint_set;
      constraint_set.add_constraint(
          SpecializedConstraint(SpecializedConstraint::VIRTUAL_SPECIALIZE));
      LayoutConstraints *constraints = 
        rt->register_layout(FieldSpace::NO_SPACE, constraint_set);
      FieldMask all_ones(LEGION_FIELD_MASK_FIELD_ALL_ONES);
      std::vector<unsigned> mask_index_map;
      std::vector<CustomSerdezID> serdez;
      std::vector<std::pair<FieldID,size_t> > field_sizes;
      LayoutDescription *layout = new LayoutDescription(all_ones, constraints);
      PointerConstraint pointer_constraint(Memory::NO_MEMORY, 0);
      singleton = new VirtualManager(rt->forest, layout, pointer_constraint,
                                     did, rt->address_space);
      // register this with the runtime
      rt->register_distributed_collectable(did, singleton);
      // put a permenant resource reference on this so it is never deleted
      singleton->add_base_resource_ref(MAX_GC_REF);
    }

    /////////////////////////////////////////////////////////////
    // Instance Builder
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    size_t InstanceBuilder::compute_needed_size(RegionTreeForest *forest)
    //--------------------------------------------------------------------------
    {
      if (!valid)
        initialize(forest);
      size_t total_field_bytes = 0;
      for (unsigned idx = 0; idx < field_sizes.size(); idx++)
        total_field_bytes += field_sizes[idx].second;
      return (total_field_bytes * instance_domain.get_volume());
    }

    //--------------------------------------------------------------------------
    PhysicalManager* InstanceBuilder::create_physical_instance(
                                                       RegionTreeForest *forest)
    //--------------------------------------------------------------------------
    {
      if (!valid)
        initialize(forest);
#ifdef NEW_INSTANCE_CREATION
      PhysicalInstance instance = PhysicalInstance::NO_INST;
      Event ready = forest->create_instance(instance_domain, 
                  memory_manager->memory, field_sizes, instance, constraints);
#else
      PhysicalInstance instance = forest->create_instance(instance_domain,
                                       memory_manager->memory, sizes_only, 
                                       block_size, redop_id, creator_id);
      Event ready = Event::NO_EVENT;
#endif
      // If we couldn't make it then we are done
      if (!instance.exists())
        return NULL;
      // Figure out what kind of instance we just made
      PhysicalManager *result = NULL;
      DistributedID did = forest->runtime->get_available_distributed_id(false);
      AddressSpaceID local_space = forest->runtime->address_space;
      FieldSpaceNode *field_node = ancestor->column_source;
      // Important implementation detail here: we pull the pointer constraint
      // out of the set of constraints here and don't include it in the layout
      // constraints so we can abstract over lots of different layouts. We'll
      // store the pointer constraint separately in the physical instance
      PointerConstraint pointer_constraint = constraints.pointer_constraint;
      constraints.pointer_constraint = PointerConstraint();
      // Now let's find the layout constraints to use for this instance
      LayoutDescription *layout = 
        field_node->find_layout_description(instance_mask, constraints);
      // If we couldn't find one then we make one
      if (layout == NULL)
      {
        // First make a new layout constraint
        LayoutConstraints *layout_constraints = 
         forest->runtime->register_layout(field_node->handle,constraints);
        // Then make our description
        layout = field_node->create_layout_description(instance_mask,
                 layout_constraints, mask_index_map, serdez, field_sizes);
      }
      switch (constraints.specialized_constraint.get_kind())
      {
        case SpecializedConstraint::NORMAL_SPECIALIZE:
          {
            
            // Now we can make the manager
            result = legion_new<InstanceManager>(forest, did, local_space,
                                                 local_space, memory_manager,
                                                 instance, instance_domain, 
                                                 own_domain, ancestor, layout, 
                                                 pointer_constraint,
                                                 ready, true/*register now*/);
            break;
          }
        case SpecializedConstraint::REDUCTION_FOLD_SPECIALIZE:
          {
#ifdef DEBUG_HIGH_LEVEL
            assert(field_sizes.size() == 1);
#endif
            result = legion_new<FoldReductionManager>(forest, did, 
                                              field_sizes[0].first, local_space,
                                              local_space, memory_manager, 
                                              instance, layout, 
                                              pointer_constraint, 
                                              instance_domain, own_domain,
                                              ancestor, redop_id,
                                              reduction_op, ready, 
                                              true/*register now*/);
            break;
          }
        case SpecializedConstraint::REDUCTION_LIST_SPECIALIZE:
          {
            // TODO: implement this
            assert(false);
            break;
          }
        default:
          assert(false); // illegal specialized case
      }
#ifdef DEBUG_HIGH_LEVEL
      assert(result != NULL);
#endif
      return result;
    }

    //--------------------------------------------------------------------------
    void InstanceBuilder::initialize(RegionTreeForest *forest)
    //--------------------------------------------------------------------------
    {
      compute_ancestor_and_domain(forest); 
#ifdef NEW_INSTANCE_CREATION
      compute_new_parameters();
#else
      compute_old_parameters();
#endif
    }

    //--------------------------------------------------------------------------
    void InstanceBuilder::compute_ancestor_and_domain(RegionTreeForest *forest)
    //--------------------------------------------------------------------------
    {
      // Check to see if they are all empty, in which case we will make
      // an empty instance with its ancestor being the root of the region
      // tree so it can satisfy all empty regions in this region tree safely
      std::vector<RegionNode*> non_empty_regions;
      std::vector<const Domain*> non_empty_domains;
      for (unsigned idx = 0; idx < regions.size(); idx++)
      {
        RegionNode *next = forest->get_node(regions[idx]);
        const Domain &next_domain = next->get_domain_blocking();
        // Check for empty
        size_t volume = next_domain.get_volume();
        if (volume == 0)
        {
          // Do something special if we know we aren't going to have
          // any non-empty regions
          if ((idx == (regions.size()-1)) && non_empty_regions.empty())
          {
            // We're going to make an empty instance which is all fine, but
            // in order to be sound for other parts of the analysis, we need
            // the ancestor to be the root of the region tree so that this
            // instance can be safely used for any empty region in this tree.
            instance_domain = next_domain;
            while (next->parent != NULL)
              next = next->parent->parent;
            ancestor = next;
            return;
          }
          continue;
        }
        non_empty_regions.push_back(next);
        non_empty_domains.push_back(&next_domain);
      }
      // At this point we have at least one non-empty region
#ifdef DEBUG_HIGH_LEVEL
      assert(!non_empty_regions.empty());
#endif
      ancestor = non_empty_regions[0];
      if (non_empty_regions.size() > 1)
      {
        // Compute an union of the all the index spaces for the basis
        // and the common ancestor of all regions
        const Domain *first = non_empty_domains[0];
        switch (first->get_dim())
        {
          case 0:
            {
              Realm::ElementMask result = 
                first->get_index_space().get_valid_mask();
              for (unsigned idx = 1; idx < non_empty_regions.size(); idx++)
              {
                RegionNode *next = non_empty_regions[idx];
                const Domain *next_domain = non_empty_domains[idx];
                result |= next_domain->get_index_space().get_valid_mask();
                // Find the common ancestor
                ancestor = find_common_ancestor(ancestor, next);
              }
              instance_domain = Domain(
                  Realm::IndexSpace::create_index_space(result));
              own_domain = true;
              break;
            }
          case 1:
            {
              LegionRuntime::Arrays::Rect<1> result = first->get_rect<1>();
              for (unsigned idx = 1; idx < non_empty_regions.size(); idx++)
              {
                RegionNode *next = non_empty_regions[idx];
                const Domain *next_domain = non_empty_domains[idx];
                LegionRuntime::Arrays::Rect<1> next_rect = 
                  next_domain->get_rect<1>();
                result = result.convex_hull(next_rect);
                // Find the common ancesstor
                ancestor = find_common_ancestor(ancestor, next); 
              }
              instance_domain = Domain::from_rect<1>(result);
              break;
            }
          case 2:
            {
              LegionRuntime::Arrays::Rect<2> result = first->get_rect<2>();
              for (unsigned idx = 1; idx < non_empty_regions.size(); idx++)
              {
                RegionNode *next = non_empty_regions[idx];
                const Domain *next_domain = non_empty_domains[idx];
                LegionRuntime::Arrays::Rect<2> next_rect = 
                  next_domain->get_rect<2>();
                result = result.convex_hull(next_rect);
                // Find the common ancesstor
                ancestor = find_common_ancestor(ancestor, next); 
              }
              instance_domain = Domain::from_rect<2>(result);
              break;
            }
          case 3:
            {
              LegionRuntime::Arrays::Rect<3> result = first->get_rect<3>();
              for (unsigned idx = 1; idx < non_empty_regions.size(); idx++)
              {
                RegionNode *next = non_empty_regions[idx];
                const Domain *next_domain = non_empty_domains[idx]; 
                LegionRuntime::Arrays::Rect<3> next_rect = 
                  next_domain->get_rect<3>();
                result = result.convex_hull(next_rect);
                // Find the common ancesstor
                ancestor = find_common_ancestor(ancestor, next); 
              }
              instance_domain = Domain::from_rect<3>(result);
              break;
            }
          default:
            assert(false); // unsupported number of dimensions
        }
      }
      else
        instance_domain = *(non_empty_domains[0]);
    }

    //--------------------------------------------------------------------------
    RegionNode* InstanceBuilder::find_common_ancestor(RegionNode *one,
                                                      RegionNode *two) const
    //--------------------------------------------------------------------------
    {
      // Make them the same level
      while (one->row_source->depth > two->row_source->depth)
      {
#ifdef DEBUG_HIGH_LEVEL
        assert(one->parent != NULL);
#endif
        one = one->parent->parent;
      }
      while (one->row_source->depth < two->row_source->depth)
      {
#ifdef DEBUG_HIGH_LEVEL
        assert(two->parent != NULL);
#endif
        two = two->parent->parent;
      }
      // While they are not the same, make them both go up
      while (one != two)
      {
#ifdef DEBUG_HIGH_LEVEL
        assert(one->parent != NULL);
        assert(two->parent != NULL);
#endif
        one = one->parent->parent;
        two = two->parent->parent;
      }
      return one;
    }

    //--------------------------------------------------------------------------
    void InstanceBuilder::compute_new_parameters(void)
    //--------------------------------------------------------------------------
    {
      FieldSpaceNode *field_node = ancestor->column_source;      
      const std::vector<FieldID> &field_set = 
        constraints.field_constraint.get_field_set(); 
      field_sizes.resize(field_set.size());
      mask_index_map.resize(field_set.size());
      serdez.resize(field_set.size());
      field_node->compute_create_offsets(field_set, field_sizes,
                                         mask_index_map, serdez, instance_mask);
    }

    //--------------------------------------------------------------------------
    void InstanceBuilder::compute_old_parameters(void)
    //--------------------------------------------------------------------------
    {
      FieldSpaceNode *field_node = ancestor->column_source;      
      const std::vector<FieldID> &field_set = 
        constraints.field_constraint.get_field_set(); 
      field_sizes.resize(field_set.size());
      mask_index_map.resize(field_set.size());
      serdez.resize(field_set.size());
      field_node->compute_create_offsets(field_set, field_sizes,
                                         mask_index_map, serdez, instance_mask);
      sizes_only.resize(field_sizes.size());
      for (unsigned idx = 0; idx < field_sizes.size(); idx++)
        sizes_only[idx] = field_sizes[idx].second;
      // Now figure out what kind of instance we're going to make, look at
      // the constraints and see if we recognize any of them
      switch (constraints.specialized_constraint.get_kind())
      {
        case SpecializedConstraint::NORMAL_SPECIALIZE:
          {
            const std::vector<DimensionKind> &ordering = 
                                      constraints.ordering_constraint.ordering;
            size_t max_block_size = instance_domain.get_volume();
            // I hate unstructured index spaces
            if (instance_domain.get_dim() == 0)
              max_block_size = instance_domain.get_index_space().
                                              get_valid_mask().get_num_elmts();
            // See if we are making an AOS or SOA instance
            if (!ordering.empty())
            {
              // If fields are first, it is AOS if the fields
              // are last it is SOA, otherwise, see if we can find
              // fields in which case we can't support it yet
              if (ordering.front() == DIM_F)
                block_size = 1;
              else if (ordering.back() == DIM_F)
                block_size = max_block_size;
              else
              {
                for (unsigned idx = 0; idx < ordering.size(); idx++)
                {
                  if (ordering[idx] == DIM_F)
                    assert(false); // need to handle this case
                }
                block_size = max_block_size;
              }
            }
            else
              block_size = max_block_size;
            // redop id is already zero
            break;
          }
        case SpecializedConstraint::REDUCTION_FOLD_SPECIALIZE:
          {
            block_size = 1;
            redop_id = constraints.specialized_constraint.get_reduction_op();
            reduction_op = Runtime::get_reduction_op(redop_id);
            break;
          }
        case SpecializedConstraint::REDUCTION_LIST_SPECIALIZE:
          {
            // TODO: implement list reduction instances
            assert(false);
            redop_id = constraints.specialized_constraint.get_reduction_op();
            reduction_op = Runtime::get_reduction_op(redop_id);
            break;
          }
        case SpecializedConstraint::VIRTUAL_SPECIALIZE:
          {
            log_run.error("Illegal request to create a virtual instance");
            assert(false);
          }
        default:
          assert(false); // unknown kind
      }
    }

  }; // namespace Internal 
}; // namespace Legion

