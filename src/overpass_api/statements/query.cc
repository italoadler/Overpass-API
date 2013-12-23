/** Copyright 2008, 2009, 2010, 2011, 2012 Roland Olbricht
*
* This file is part of Overpass_API.
*
* Overpass_API is free software: you can redistribute it and/or modify
* it under the terms of the GNU Affero General Public License as
* published by the Free Software Foundation, either version 3 of the
* License, or (at your option) any later version.
*
* Overpass_API is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU Affero General Public License
* along with Overpass_API.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "../../template_db/block_backend.h"
#include "../../template_db/random_file.h"
#include "../core/settings.h"
#include "../data/abstract_processing.h"
#include "../data/collect_members.h"
#include "../data/filenames.h"
#include "../data/regular_expression.h"
#include "meta_collector.h"
#include "area_query.h"
#include "query.h"

#include <algorithm>
#include <sstream>

using namespace std;

//-----------------------------------------------------------------------------

bool Query_Statement::area_query_exists_ = false;

Generic_Statement_Maker< Query_Statement > Query_Statement::statement_maker("query");

Query_Statement::Query_Statement
    (int line_number_, const map< string, string >& input_attributes, Query_Constraint* bbox_limitation)
    : Output_Statement(line_number_)
{
  map< string, string > attributes;
  
  attributes["into"] = "_";
  attributes["type"] = "";
  
  eval_attributes_array(get_name(), attributes, input_attributes);
  
  set_output(attributes["into"]);
  if (attributes["type"] == "node")
    type = QUERY_NODE;
  else if (attributes["type"] == "way")
    type = QUERY_WAY;
  else if (attributes["type"] == "relation")
    type = QUERY_RELATION;
  else if (attributes["type"] == "area")
  {
    type = QUERY_AREA;
    area_query_exists_ = true;
  }
  else
  {
    type = 0;
    ostringstream temp;
    temp<<"For the attribute \"type\" of the element \"query\""
	<<" the only allowed values are \"node\", \"way\", \"relation\" or \"area\".";
    add_static_error(temp.str());
  }
  
  if (bbox_limitation)
    constraints.push_back(bbox_limitation);
}

void Query_Statement::add_statement(Statement* statement, string text)
{
  assure_no_text(text, this->get_name());
  
  Has_Kv_Statement* has_kv(dynamic_cast<Has_Kv_Statement*>(statement));
  if (has_kv)
  {
    if (has_kv->get_value() != "")
    {
      if (has_kv->get_straight())
        key_values.push_back(make_pair< string, string >
	    (has_kv->get_key(), has_kv->get_value()));
      else
        key_nvalues.push_back(make_pair< string, string >
	    (has_kv->get_key(), has_kv->get_value()));
    }
    else if (has_kv->get_regex())
    {
      if (has_kv->get_straight())
	key_regexes.push_back(make_pair< string, Regular_Expression* >
            (has_kv->get_key(), has_kv->get_regex()));
      else
	key_nregexes.push_back(make_pair< string, Regular_Expression* >
            (has_kv->get_key(), has_kv->get_regex()));
    }
    else
      keys.push_back(has_kv->get_key());
    return;
  }
  
  Query_Constraint* constraint = statement->get_query_constraint();
  if (constraint)
    constraints.push_back(constraint);
  else
    substatement_error(get_name(), statement);
}


set< Tag_Index_Global > get_kv_req(const string& key, const string& value)
{
  set< Tag_Index_Global > result;
  Tag_Index_Global idx;
  idx.key = key;
  idx.value = value;
  result.insert(idx);
  return result;
}


set< pair< Tag_Index_Global, Tag_Index_Global > > get_k_req(const string& key)
{
  set< pair< Tag_Index_Global, Tag_Index_Global > > result;
  pair< Tag_Index_Global, Tag_Index_Global > idx_pair;
  idx_pair.first.key = key;
  idx_pair.first.value = "";
  idx_pair.second.key = key + (char)0;
  idx_pair.second.value = "";
  result.insert(idx_pair);
  return result;
}


template< class Id_Type >
std::map< Id_Type, uint64 > collect_attic_kv(
    vector< pair< string, string > >::const_iterator kvit, uint64 timestamp,
    Block_Backend< Tag_Index_Global, Tag_Object_Global< Id_Type > >& tags_db,
    Block_Backend< Tag_Index_Global, Attic< Tag_Object_Global< Id_Type > > >& attic_tags_db)
{
  std::map< Id_Type, uint64 > timestamp_per_id;
  set< Tag_Index_Global > tag_req = get_kv_req(kvit->first, kvit->second);
      
  for (typename Block_Backend< Tag_Index_Global, Tag_Object_Global< Id_Type > >::Discrete_Iterator
      it2(tags_db.discrete_begin(tag_req.begin(), tag_req.end()));
      !(it2 == tags_db.discrete_end()); ++it2)
    timestamp_per_id[it2.object().id] = NOW;
      
  for (typename Block_Backend< Tag_Index_Global, Attic< Tag_Object_Global< Id_Type > > >::Discrete_Iterator
      it2(attic_tags_db.discrete_begin(tag_req.begin(), tag_req.end()));
      !(it2 == attic_tags_db.discrete_end()); ++it2)
  {
    if (it2.object().timestamp > timestamp)
    {
      uint64& ref = timestamp_per_id[it2.object().id];
      if (ref == 0 || it2.object().timestamp < ref)
        ref = it2.object().timestamp;
    }
  }
      
  set< pair< Tag_Index_Global, Tag_Index_Global > > range_req = get_k_req(kvit->first);
      
  for (typename Block_Backend< Tag_Index_Global, Attic< Tag_Object_Global< Id_Type > > >::Range_Iterator
      it2(attic_tags_db.range_begin(Default_Range_Iterator< Tag_Index_Global >(range_req.begin()),
          Default_Range_Iterator< Tag_Index_Global >(range_req.end())));
      !(it2 == attic_tags_db.range_end()); ++it2)
  {
    if (it2.object().timestamp > timestamp)
    {
      typename std::map< Id_Type, uint64 >::iterator it = timestamp_per_id.find(it2.object().id);
      if (it != timestamp_per_id.end())
      {
        if (it2.object().timestamp < it->second)
          timestamp_per_id.erase(it);
      }
    }
  }
  
  return timestamp_per_id;
}


template< class Id_Type >
std::map< Id_Type, uint64 > collect_attic_k(
    vector< string >::const_iterator kit, uint64 timestamp,
    Block_Backend< Tag_Index_Global, Tag_Object_Global< Id_Type > >& tags_db,
    Block_Backend< Tag_Index_Global, Attic< Tag_Object_Global< Id_Type > > >& attic_tags_db)
{
  std::map< Id_Type, uint64 > timestamp_per_id;
  set< pair< Tag_Index_Global, Tag_Index_Global > > range_req = get_k_req(*kit);
      
  for (typename Block_Backend< Tag_Index_Global, Tag_Object_Global< Id_Type > >::Range_Iterator
      it2(tags_db.range_begin
        (Default_Range_Iterator< Tag_Index_Global >(range_req.begin()),
      Default_Range_Iterator< Tag_Index_Global >(range_req.end())));
      !(it2 == tags_db.range_end()); ++it2)
    timestamp_per_id[it2.object().id] = NOW;
      
  for (typename Block_Backend< Tag_Index_Global, Attic< Tag_Object_Global< Id_Type > > >::Range_Iterator
      it2(attic_tags_db.range_begin
        (Default_Range_Iterator< Tag_Index_Global >(range_req.begin()),
      Default_Range_Iterator< Tag_Index_Global >(range_req.end())));
      !(it2 == attic_tags_db.range_end()); ++it2)
  {
    if (it2.object().timestamp > timestamp && it2.index().value != void_tag_value())
    {
      uint64& ref = timestamp_per_id[it2.object().id];
      if (ref == 0 || it2.object().timestamp < ref)
        ref = it2.object().timestamp;
    }
  }
      
  for (typename Block_Backend< Tag_Index_Global, Attic< Tag_Object_Global< Id_Type > > >::Range_Iterator
      it2(attic_tags_db.range_begin(Default_Range_Iterator< Tag_Index_Global >(range_req.begin()),
          Default_Range_Iterator< Tag_Index_Global >(range_req.end())));
      !(it2 == attic_tags_db.range_end()); ++it2)
  {
    if (it2.object().timestamp > timestamp && it2.index().value == void_tag_value())
    {
      typename std::map< Id_Type, uint64 >::iterator it = timestamp_per_id.find(it2.object().id);
      if (it != timestamp_per_id.end())
      {
        if (it2.object().timestamp < it->second)
          timestamp_per_id.erase(it);
      }
    }
  }
  
  return timestamp_per_id;
}


template< class Id_Type >
std::map< Id_Type, uint64 > collect_attic_kregv(
    vector< pair< string, Regular_Expression* > >::const_iterator krit, uint64 timestamp,
    Block_Backend< Tag_Index_Global, Tag_Object_Global< Id_Type > >& tags_db,
    Block_Backend< Tag_Index_Global, Attic< Tag_Object_Global< Id_Type > > >& attic_tags_db)
{
  std::map< Id_Type, uint64 > timestamp_per_id;
  set< pair< Tag_Index_Global, Tag_Index_Global > > range_req = get_k_req(krit->first);
      
  for (typename Block_Backend< Tag_Index_Global, Tag_Object_Global< Id_Type > >::Range_Iterator
      it2(tags_db.range_begin
        (Default_Range_Iterator< Tag_Index_Global >(range_req.begin()),
      Default_Range_Iterator< Tag_Index_Global >(range_req.end())));
      !(it2 == tags_db.range_end()); ++it2)
  {
    if (krit->second->matches(it2.index().value))
      timestamp_per_id[it2.object().id] = NOW;
  }
      
  for (typename Block_Backend< Tag_Index_Global, Attic< Tag_Object_Global< Id_Type > > >::Range_Iterator
      it2(attic_tags_db.range_begin
        (Default_Range_Iterator< Tag_Index_Global >(range_req.begin()),
      Default_Range_Iterator< Tag_Index_Global >(range_req.end())));
      !(it2 == attic_tags_db.range_end()); ++it2)
  {
    if (it2.object().timestamp > timestamp && krit->second->matches(it2.index().value))
    {
      uint64& ref = timestamp_per_id[it2.object().id];
      if (ref == 0 || it2.object().timestamp < ref)
        ref = it2.object().timestamp;
    }
  }
      
  for (typename Block_Backend< Tag_Index_Global, Attic< Tag_Object_Global< Id_Type > > >::Range_Iterator
      it2(attic_tags_db.range_begin(Default_Range_Iterator< Tag_Index_Global >(range_req.begin()),
          Default_Range_Iterator< Tag_Index_Global >(range_req.end())));
      !(it2 == attic_tags_db.range_end()); ++it2)
  {
    if (it2.object().timestamp > timestamp)
    {
      typename std::map< Id_Type, uint64 >::iterator it = timestamp_per_id.find(it2.object().id);
      if (it != timestamp_per_id.end())
      {
        if (it2.object().timestamp < it->second)
          timestamp_per_id.erase(it);
      }
    }
  }
  
  return timestamp_per_id;
}


template < typename T >
struct Optional
{
  Optional(T* obj_) : obj(obj_) {}
  ~Optional() { delete obj; }
  
  T* obj;
};


template< class Id_Type >
vector< Id_Type > Query_Statement::collect_ids
  (const File_Properties& file_prop, const File_Properties& attic_file_prop, Resource_Manager& rman,
   uint64 timestamp, bool check_keys_late)
{
  if (key_values.empty() && keys.empty() && key_regexes.empty())
    return vector< Id_Type >();
  
  Block_Backend< Tag_Index_Global, Tag_Object_Global< Id_Type > > tags_db
      (rman.get_transaction()->data_index(&file_prop));
  Optional< Block_Backend< Tag_Index_Global, Attic< Tag_Object_Global< Id_Type > > > > attic_tags_db
      (timestamp == NOW ? 0 :
        new Block_Backend< Tag_Index_Global, Attic< Tag_Object_Global< Id_Type > > >
        (rman.get_transaction()->data_index(&attic_file_prop)));
  
  // Handle simple Key-Value pairs
  vector< Id_Type > new_ids;
  vector< pair< string, string > >::const_iterator kvit = key_values.begin();

  if (kvit != key_values.end())
  {
    if (timestamp == NOW)
    {
      set< Tag_Index_Global > tag_req = get_kv_req(kvit->first, kvit->second);
      for (typename Block_Backend< Tag_Index_Global, Tag_Object_Global< Id_Type > >::Discrete_Iterator
          it2(tags_db.discrete_begin(tag_req.begin(), tag_req.end()));
          !(it2 == tags_db.discrete_end()); ++it2)
        new_ids.push_back(it2.object().id);

      sort(new_ids.begin(), new_ids.end());
    }
    else
    {
      std::map< Id_Type, uint64 > timestamp_per_id
          = collect_attic_kv(kvit, timestamp, tags_db, *attic_tags_db.obj);
      
      for (typename std::map< Id_Type, uint64 >::const_iterator it = timestamp_per_id.begin();
           it != timestamp_per_id.end(); ++it)
        new_ids.push_back(it->first);
    }
    rman.health_check(*this);
    ++kvit;
  }
  
  for (; kvit != key_values.end(); ++kvit)
  {
    vector< Id_Type > old_ids;
    old_ids.swap(new_ids);

    if (timestamp == NOW)
    {
      set< Tag_Index_Global > tag_req = get_kv_req(kvit->first, kvit->second);
      for (typename Block_Backend< Tag_Index_Global, Tag_Object_Global< Id_Type > >::Discrete_Iterator
          it2(tags_db.discrete_begin(tag_req.begin(), tag_req.end()));
          !(it2 == tags_db.discrete_end()); ++it2)
      {
        if (binary_search(old_ids.begin(), old_ids.end(), it2.object().id))
          new_ids.push_back(it2.object().id);
      }
      sort(new_ids.begin(), new_ids.end());
    }
    else
    {
      std::map< Id_Type, uint64 > timestamp_per_id
          = collect_attic_kv(kvit, timestamp, tags_db, *attic_tags_db.obj);
      
      for (typename std::map< Id_Type, uint64 >::const_iterator it = timestamp_per_id.begin();
           it != timestamp_per_id.end(); ++it)
      {
        if (binary_search(old_ids.begin(), old_ids.end(), it->first))
          new_ids.push_back(it->first);
      }
    }
    rman.health_check(*this);
  }

  // Handle simple Keys Only
  if (!check_keys_late)
  {
    vector< string >::const_iterator kit = keys.begin();
    if (key_values.empty() && kit != keys.end())
    {
      if (timestamp == NOW)
      {
        set< pair< Tag_Index_Global, Tag_Index_Global > > range_req = get_k_req(*kit);
        for (typename Block_Backend< Tag_Index_Global, Tag_Object_Global< Id_Type > >::Range_Iterator
            it2(tags_db.range_begin
              (Default_Range_Iterator< Tag_Index_Global >(range_req.begin()),
            Default_Range_Iterator< Tag_Index_Global >(range_req.end())));
            !(it2 == tags_db.range_end()); ++it2)
          new_ids.push_back(it2.object().id);

        sort(new_ids.begin(), new_ids.end());
      }
      else
      {
        std::map< Id_Type, uint64 > timestamp_per_id
            = collect_attic_k(kit, timestamp, tags_db, *attic_tags_db.obj);
      
        for (typename std::map< Id_Type, uint64 >::const_iterator it = timestamp_per_id.begin();
             it != timestamp_per_id.end(); ++it)
          new_ids.push_back(it->first);
      }
      rman.health_check(*this);
      ++kit;
    }
    
    for (; kit != keys.end(); ++kit)
    {
      vector< Id_Type > old_ids;
      old_ids.swap(new_ids);
      if (timestamp == NOW)
      {
        set< pair< Tag_Index_Global, Tag_Index_Global > > range_req = get_k_req(*kit);
        for (typename Block_Backend< Tag_Index_Global, Tag_Object_Global< Id_Type > >::Range_Iterator
            it2(tags_db.range_begin
              (Default_Range_Iterator< Tag_Index_Global >(range_req.begin()),
            Default_Range_Iterator< Tag_Index_Global >(range_req.end())));
            !(it2 == tags_db.range_end()); ++it2)
        {
          if (binary_search(old_ids.begin(), old_ids.end(), it2.object().id))
            new_ids.push_back(it2.object().id);
        }
        
        sort(new_ids.begin(), new_ids.end());    
      }
      else
      {
        std::map< Id_Type, uint64 > timestamp_per_id
            = collect_attic_k(kit, timestamp, tags_db, *attic_tags_db.obj);
      
        for (typename std::map< Id_Type, uint64 >::const_iterator it = timestamp_per_id.begin();
             it != timestamp_per_id.end(); ++it)
        {
          if (binary_search(old_ids.begin(), old_ids.end(), it->first))
            new_ids.push_back(it->first);
        }
      }
      rman.health_check(*this);
    }

    // Handle Key-Regular-Expression-Value pairs
    vector< pair< string, Regular_Expression* > >::const_iterator krit = key_regexes.begin();
    if (key_values.empty() && (keys.empty() || check_keys_late) && krit != key_regexes.end())
    {
      if (timestamp == NOW)
      {
        set< pair< Tag_Index_Global, Tag_Index_Global > > range_req = get_k_req(krit->first);
        for (typename Block_Backend< Tag_Index_Global, Tag_Object_Global< Id_Type > >::Range_Iterator
            it2(tags_db.range_begin
              (Default_Range_Iterator< Tag_Index_Global >(range_req.begin()),
            Default_Range_Iterator< Tag_Index_Global >(range_req.end())));
            !(it2 == tags_db.range_end()); ++it2)
        {
          if (krit->second->matches(it2.index().value))
            new_ids.push_back(it2.object().id);
        }

        sort(new_ids.begin(), new_ids.end());
      }
      else
      {
        std::map< Id_Type, uint64 > timestamp_per_id
            = collect_attic_kregv(krit, timestamp, tags_db, *attic_tags_db.obj);
      
        for (typename std::map< Id_Type, uint64 >::const_iterator it = timestamp_per_id.begin();
             it != timestamp_per_id.end(); ++it)
          new_ids.push_back(it->first);
      }
      rman.health_check(*this);
      ++krit;
    }
  
    for (; krit != key_regexes.end(); ++krit)
    {
      vector< Id_Type > old_ids;
      old_ids.swap(new_ids);
      if (timestamp == NOW)
      {
        set< pair< Tag_Index_Global, Tag_Index_Global > > range_req = get_k_req(krit->first);
        for (typename Block_Backend< Tag_Index_Global, Tag_Object_Global< Id_Type > >::Range_Iterator
            it2(tags_db.range_begin
            (Default_Range_Iterator< Tag_Index_Global >(range_req.begin()),
             Default_Range_Iterator< Tag_Index_Global >(range_req.end())));
            !(it2 == tags_db.range_end()); ++it2)
        {
          if (binary_search(old_ids.begin(), old_ids.end(), it2.object().id)
              && krit->second->matches(it2.index().value))
            new_ids.push_back(it2.object().id);
        }
        
        sort(new_ids.begin(), new_ids.end());    
      }
      else
      {
        std::map< Id_Type, uint64 > timestamp_per_id
            = collect_attic_kregv(krit, timestamp, tags_db, *attic_tags_db.obj);
      
        for (typename std::map< Id_Type, uint64 >::const_iterator it = timestamp_per_id.begin();
             it != timestamp_per_id.end(); ++it)
        {
          if (binary_search(old_ids.begin(), old_ids.end(), it->first))
            new_ids.push_back(it->first);
        }
      }
      rman.health_check(*this);
    }
  }

  return new_ids;
}


template< class Id_Type >
vector< Id_Type > Query_Statement::collect_ids
  (const File_Properties& file_prop, Resource_Manager& rman,
   bool check_keys_late)
{
  if (key_values.empty() && keys.empty() && key_regexes.empty())
    return vector< Id_Type >();
 
  Block_Backend< Tag_Index_Global, Id_Type > tags_db
      (rman.get_transaction()->data_index(&file_prop));
  
  // Handle simple Key-Value pairs
  vector< Id_Type > new_ids;
  vector< pair< string, string > >::const_iterator kvit = key_values.begin();

  if (kvit != key_values.end())
  {
    set< Tag_Index_Global > tag_req = get_kv_req(kvit->first, kvit->second);
    for (typename Block_Backend< Tag_Index_Global, Id_Type >::Discrete_Iterator
        it2(tags_db.discrete_begin(tag_req.begin(), tag_req.end()));
        !(it2 == tags_db.discrete_end()); ++it2)
      new_ids.push_back(it2.object());

    sort(new_ids.begin(), new_ids.end());
    rman.health_check(*this);
    ++kvit;
  }
  
  for (; kvit != key_values.end(); ++kvit)
  {
    vector< Id_Type > old_ids;
    old_ids.swap(new_ids);

    set< Tag_Index_Global > tag_req = get_kv_req(kvit->first, kvit->second);
    for (typename Block_Backend< Tag_Index_Global, Id_Type >::Discrete_Iterator
        it2(tags_db.discrete_begin(tag_req.begin(), tag_req.end()));
        !(it2 == tags_db.discrete_end()); ++it2)
    {
      if (binary_search(old_ids.begin(), old_ids.end(), it2.object()))
        new_ids.push_back(it2.object());
    }
    sort(new_ids.begin(), new_ids.end());    
    rman.health_check(*this);
  }

  // Handle simple Keys Only
  if (!check_keys_late)
  {
    vector< string >::const_iterator kit = keys.begin();
    if (key_values.empty() && kit != keys.end())
    {
      set< pair< Tag_Index_Global, Tag_Index_Global > > range_req = get_k_req(*kit);
      for (typename Block_Backend< Tag_Index_Global, Id_Type >::Range_Iterator
          it2(tags_db.range_begin
            (Default_Range_Iterator< Tag_Index_Global >(range_req.begin()),
           Default_Range_Iterator< Tag_Index_Global >(range_req.end())));
          !(it2 == tags_db.range_end()); ++it2)
        new_ids.push_back(it2.object());

      sort(new_ids.begin(), new_ids.end());
      rman.health_check(*this);
      ++kit;
    }
    
    for (; kit != keys.end(); ++kit)
    {
      vector< Id_Type > old_ids;
      old_ids.swap(new_ids);
      {
        set< pair< Tag_Index_Global, Tag_Index_Global > > range_req = get_k_req(*kit);
        for (typename Block_Backend< Tag_Index_Global, Id_Type >::Range_Iterator
            it2(tags_db.range_begin
              (Default_Range_Iterator< Tag_Index_Global >(range_req.begin()),
            Default_Range_Iterator< Tag_Index_Global >(range_req.end())));
            !(it2 == tags_db.range_end()); ++it2)
        {
          if (binary_search(old_ids.begin(), old_ids.end(), it2.object()))
            new_ids.push_back(it2.object());
        }
      }
      sort(new_ids.begin(), new_ids.end());    
      rman.health_check(*this);
    }

    // Handle Key-Regular-Expression-Value pairs
    vector< pair< string, Regular_Expression* > >::const_iterator krit = key_regexes.begin();
    if (key_values.empty() && (keys.empty() || check_keys_late) && krit != key_regexes.end())
    {
      set< pair< Tag_Index_Global, Tag_Index_Global > > range_req = get_k_req(krit->first);
      for (typename Block_Backend< Tag_Index_Global, Id_Type >::Range_Iterator
          it2(tags_db.range_begin
            (Default_Range_Iterator< Tag_Index_Global >(range_req.begin()),
             Default_Range_Iterator< Tag_Index_Global >(range_req.end())));
          !(it2 == tags_db.range_end()); ++it2)
      {
        if (krit->second->matches(it2.index().value))
          new_ids.push_back(it2.object());
      }

      sort(new_ids.begin(), new_ids.end());
      rman.health_check(*this);
      ++krit;
    }
  
    for (; krit != key_regexes.end(); ++krit)
    {
      vector< Id_Type > old_ids;
      old_ids.swap(new_ids);
      {
        set< pair< Tag_Index_Global, Tag_Index_Global > > range_req = get_k_req(krit->first);
        for (typename Block_Backend< Tag_Index_Global, Id_Type >::Range_Iterator
            it2(tags_db.range_begin
            (Default_Range_Iterator< Tag_Index_Global >(range_req.begin()),
             Default_Range_Iterator< Tag_Index_Global >(range_req.end())));
            !(it2 == tags_db.range_end()); ++it2)
        {
          if (binary_search(old_ids.begin(), old_ids.end(), it2.object())
              && krit->second->matches(it2.index().value))
            new_ids.push_back(it2.object());
        }
      }
      sort(new_ids.begin(), new_ids.end());    
      rman.health_check(*this);
    }

  }

  return new_ids;
}


template< class Id_Type >
vector< Id_Type > Query_Statement::collect_non_ids
  (const File_Properties& file_prop, const File_Properties& attic_file_prop,
   Resource_Manager& rman, uint64 timestamp)
{
  if (key_nvalues.empty() && key_nregexes.empty())
    return vector< Id_Type >();
 
  Block_Backend< Tag_Index_Global, Tag_Object_Global< Id_Type > > tags_db
      (rman.get_transaction()->data_index(&file_prop));
  Optional< Block_Backend< Tag_Index_Global, Attic< Tag_Object_Global< Id_Type > > > > attic_tags_db
      (timestamp == NOW ? 0 :
        new Block_Backend< Tag_Index_Global, Attic< Tag_Object_Global< Id_Type > > >
        (rman.get_transaction()->data_index(&attic_file_prop)));
  
  vector< Id_Type > new_ids;

  // Handle Key-Non-Value pairs
  for (vector< pair< string, string > >::const_iterator knvit = key_nvalues.begin();
      knvit != key_nvalues.end(); ++knvit)
  {
    vector< Id_Type > old_ids;
    old_ids.swap(new_ids);

    if (timestamp == NOW)
    {
      set< Tag_Index_Global > tag_req = get_kv_req(knvit->first, knvit->second);
      for (typename Block_Backend< Tag_Index_Global, Tag_Object_Global< Id_Type > >::Discrete_Iterator
          it2(tags_db.discrete_begin(tag_req.begin(), tag_req.end()));
          !(it2 == tags_db.discrete_end()); ++it2)
        new_ids.push_back(it2.object().id);
    }
    else
    {
      std::map< Id_Type, uint64 > timestamp_per_id
          = collect_attic_kv(knvit, timestamp, tags_db, *attic_tags_db.obj);
      
      for (typename std::map< Id_Type, uint64 >::const_iterator it = timestamp_per_id.begin();
          it != timestamp_per_id.end(); ++it)
        new_ids.push_back(it->first);
    }
    rman.health_check(*this);
  }

  // Handle Key-Regular-Expression-Non-Value pairs  
  for (vector< pair< string, Regular_Expression* > >::const_iterator knrit = key_nregexes.begin();
      knrit != key_nregexes.end(); ++knrit)
  {
    vector< Id_Type > old_ids;
    old_ids.swap(new_ids);
    if (timestamp == NOW)
    {
      set< pair< Tag_Index_Global, Tag_Index_Global > > range_req = get_k_req(knrit->first);
      for (typename Block_Backend< Tag_Index_Global, Tag_Object_Global< Id_Type > >::Range_Iterator
          it2(tags_db.range_begin
          (Default_Range_Iterator< Tag_Index_Global >(range_req.begin()),
           Default_Range_Iterator< Tag_Index_Global >(range_req.end())));
          !(it2 == tags_db.range_end()); ++it2)
        new_ids.push_back(it2.object().id);
    }
    else
    {
      std::map< Id_Type, uint64 > timestamp_per_id
          = collect_attic_kregv(knrit, timestamp, tags_db, *attic_tags_db.obj);
      
      for (typename std::map< Id_Type, uint64 >::const_iterator it = timestamp_per_id.begin();
          it != timestamp_per_id.end(); ++it)
        new_ids.push_back(it->first);
    }
    rman.health_check(*this);
  }

  sort(new_ids.begin(), new_ids.end());
  new_ids.erase(unique(new_ids.begin(), new_ids.end()), new_ids.end());

  return new_ids;
}


template< class Id_Type >
vector< Id_Type > Query_Statement::collect_non_ids
  (const File_Properties& file_prop, Resource_Manager& rman)
{
  if (key_nvalues.empty() && key_nregexes.empty())
    return vector< Id_Type >();
 
  Block_Backend< Tag_Index_Global, Tag_Object_Global< Id_Type > > tags_db
      (rman.get_transaction()->data_index(&file_prop));
  
  vector< Id_Type > new_ids;

  // Handle Key-Non-Value pairs
  for (vector< pair< string, string > >::const_iterator knvit = key_nvalues.begin();
      knvit != key_nvalues.end(); ++knvit)
  {
    set< pair< Tag_Index_Global, Tag_Index_Global > > range_req = get_k_req(knvit->first);
    for (typename Block_Backend< Tag_Index_Global, Tag_Object_Global< Id_Type > >::Range_Iterator
        it2(tags_db.range_begin
        (Default_Range_Iterator< Tag_Index_Global >(range_req.begin()),
         Default_Range_Iterator< Tag_Index_Global >(range_req.end())));
        !(it2 == tags_db.range_end()); ++it2)
    {
      if (it2.index().value == knvit->second)
        new_ids.push_back(it2.object().id);
    }
      
    rman.health_check(*this);
  }

  // Handle Key-Regular-Expression-Non-Value pairs  
  for (vector< pair< string, Regular_Expression* > >::const_iterator knrit = key_nregexes.begin();
      knrit != key_nregexes.end(); ++knrit)
  {
    set< pair< Tag_Index_Global, Tag_Index_Global > > range_req = get_k_req(knrit->first);
    for (typename Block_Backend< Tag_Index_Global, Tag_Object_Global< Id_Type > >::Range_Iterator
        it2(tags_db.range_begin
        (Default_Range_Iterator< Tag_Index_Global >(range_req.begin()),
         Default_Range_Iterator< Tag_Index_Global >(range_req.end())));
        !(it2 == tags_db.range_end()); ++it2)
    {
      if (knrit->second->matches(it2.index().value))
        new_ids.push_back(it2.object().id);
    }

    rman.health_check(*this);
  }

  sort(new_ids.begin(), new_ids.end());
  new_ids.erase(unique(new_ids.begin(), new_ids.end()), new_ids.end());

  return new_ids;
}


void Query_Statement::get_elements_by_id_from_db
    (map< Uint31_Index, vector< Area_Skeleton > >& elements,
     const vector< Area_Skeleton::Id_Type >& ids, bool invert_ids,
     Resource_Manager& rman, File_Properties& file_prop)
{
  if (!invert_ids)
    collect_items_flat(*this, rman, file_prop,
			Id_Predicate< Area_Skeleton >(ids), elements);
  else
    collect_items_flat(*this, rman, file_prop,
			Not_Predicate< Area_Skeleton, Id_Predicate< Area_Skeleton > >
			(Id_Predicate< Area_Skeleton >(ids)), elements);
}


template < typename TIndex, typename Id_Type >
set< pair< TIndex, TIndex > > Query_Statement::get_ranges_by_id_from_db
    (const vector< Id_Type >& ids,
     Resource_Manager& rman, File_Properties& file_prop)
{
  rman.health_check(*this, ids.size()/1000, ids.size()*60);
  set< pair< TIndex, TIndex > > range_req;
  {
    Random_File< TIndex > random
        (rman.get_transaction()->random_index(&file_prop));
    uint count = 0;
    for (typename vector< Id_Type >::const_iterator it(ids.begin()); it != ids.end(); ++it)
    {
      range_req.insert(make_pair(random.get(it->val()), TIndex(random.get(it->val()).val()+1)));
      if (++count >= 1000)
      {
	rman.health_check(*this);
	count = 0;
      }
    }
  }
  return range_req;
}


template< typename TIndex, typename TObject >
void clear_empty_indices
    (map< TIndex, vector< TObject > >& modify)
{
  for (typename map< TIndex, vector< TObject > >::iterator it = modify.begin();
      it != modify.end();)
  {
    if (!it->second.empty())
    {
      ++it;
      continue;
    }
    typename map< TIndex, vector< TObject > >::iterator next_it = it;
    if (++next_it == modify.end())
    {
      modify.erase(it);
      break;
    }
    else
    {
      TIndex idx = next_it->first;
      modify.erase(it);
      it = modify.find(idx);
    }
  }
}


template< typename Id_Type >
void filter_ids_by_tags
  (const map< string, vector< Regular_Expression* > >& keys,
   const Block_Backend< Tag_Index_Local, Id_Type >& items_db,
   typename Block_Backend< Tag_Index_Local, Id_Type >::Range_Iterator& tag_it,
   uint32 coarse_index,
   vector< Id_Type >& new_ids)
{
  vector< Id_Type > old_ids;
  string last_key, last_value;  
  bool relevant = false;
  bool valid = false;
  map< string, vector< Regular_Expression* > >::const_iterator key_it = keys.begin();
  
  while ((!(tag_it == items_db.range_end())) &&
      (((tag_it.index().index) & 0x7fffff00) == coarse_index))
  {
    if (tag_it.index().key != last_key)
    {
      last_value = "";
      
      if (relevant)
        ++key_it;
      relevant = false;
      
      if (key_it == keys.end())
	break;
      
      last_key = tag_it.index().key;
      if (last_key >= key_it->first)
      {
	if (last_key > key_it->first)
          // There are keys missing for all objects with this index. Drop all.
	  break;

	relevant = true;
	old_ids.clear();
        old_ids.swap(new_ids);
        sort(old_ids.begin(), old_ids.end());
      }
    }
    
    if (relevant)
    {
      if (tag_it.index().value != last_value)
      {
	valid = true;
	for (vector< Regular_Expression* >::const_iterator rit = key_it->second.begin();
	    rit != key_it->second.end(); ++rit)
	  valid &= (*rit)->matches(tag_it.index().value);
	last_value = tag_it.index().value;
      }
      
      if (valid && binary_search(old_ids.begin(), old_ids.end(), tag_it.object()))
        new_ids.push_back(tag_it.object());
    }

    ++tag_it;
  }
  while ((!(tag_it == items_db.range_end())) &&
      (((tag_it.index().index) & 0x7fffff00) == coarse_index))
    ++tag_it;

  if (relevant && key_it != keys.end())
    ++key_it;
  if (key_it != keys.end())
    // There are keys missing for all objects with this index. Drop all.
    new_ids.clear();
  
  sort(new_ids.begin(), new_ids.end());
}


template< typename Id_Type >
void filter_ids_by_tags
  (map< uint32, vector< Id_Type > >& ids_by_coarse,
   const map< string, vector< Regular_Expression* > >& keys,
   const Block_Backend< Tag_Index_Local, Id_Type >& items_db,
   typename Block_Backend< Tag_Index_Local, Id_Type >::Range_Iterator& tag_it,
   uint32 coarse_index)
{
  vector< Id_Type > new_ids = ids_by_coarse[coarse_index & 0x7fffff00];
  
  filter_ids_by_tags(keys, items_db, tag_it, coarse_index & 0x7fffff00, new_ids);

  new_ids.swap(ids_by_coarse[coarse_index & 0x7fffff00]);
    
  filter_ids_by_tags(keys, items_db, tag_it, coarse_index | 0x80000000, new_ids);

  vector< Id_Type > old_ids;
  old_ids.swap(ids_by_coarse[coarse_index & 0x7fffff00]);
  set_union(old_ids.begin(), old_ids.end(), new_ids.begin(), new_ids.end(),
      back_inserter(ids_by_coarse[coarse_index & 0x7fffff00]));
}


template< typename Id_Type >
void filter_ids_by_ntags
  (const map< string, pair< vector< Regular_Expression* >, vector< string > > >& keys,
   const Block_Backend< Tag_Index_Local, Id_Type >& items_db,
   typename Block_Backend< Tag_Index_Local, Id_Type >::Range_Iterator& tag_it,
   uint32 coarse_index,
   vector< Id_Type >& new_ids)
{
  vector< Id_Type > removed_ids;
  string last_key, last_value;  
  bool relevant = false;
  bool valid = false;
  map< string, pair< vector< Regular_Expression* >, vector< string > > >::const_iterator
      key_it = keys.begin();
  
  while ((!(tag_it == items_db.range_end())) &&
      (((tag_it.index().index) & 0x7fffff00) == coarse_index))
  {
    if (tag_it.index().key != last_key)
    {
      last_value = "";
      
      if (relevant)
      {
        ++key_it;
        sort(removed_ids.begin(), removed_ids.end());
        removed_ids.erase(unique(removed_ids.begin(), removed_ids.end()), removed_ids.end());
        new_ids.erase(set_difference(new_ids.begin(), new_ids.end(),
                      removed_ids.begin(), removed_ids.end(), new_ids.begin()), new_ids.end());
      }
      relevant = false;
      
      last_key = tag_it.index().key;
      while (key_it != keys.end() && last_key > key_it->first)
        ++key_it;
      
      if (key_it == keys.end())
        break;
      
      if (last_key == key_it->first)
      {
        relevant = true;
        removed_ids.clear();
      }
    }
    
    if (relevant)
    {
      if (tag_it.index().value != last_value)
      {
        valid = false;
        for (vector< Regular_Expression* >::const_iterator rit = key_it->second.first.begin();
            rit != key_it->second.first.end(); ++rit)
          valid |= (*rit)->matches(tag_it.index().value);
        for (vector< string >::const_iterator rit = key_it->second.second.begin();
            rit != key_it->second.second.end(); ++rit)
          valid |= (*rit == tag_it.index().value);
        last_value = tag_it.index().value;
      }
      
      if (valid)
        removed_ids.push_back(tag_it.object());
    }

    ++tag_it;
  }
  while ((!(tag_it == items_db.range_end())) &&
      (((tag_it.index().index) & 0x7fffff00) == coarse_index))
    ++tag_it;
  
  sort(removed_ids.begin(), removed_ids.end());
  removed_ids.erase(unique(removed_ids.begin(), removed_ids.end()), removed_ids.end());
  new_ids.erase(set_difference(new_ids.begin(), new_ids.end(),
                removed_ids.begin(), removed_ids.end(), new_ids.begin()), new_ids.end());
  
  sort(new_ids.begin(), new_ids.end());
}


template< typename Id_Type >
void filter_ids_by_ntags
  (map< uint32, vector< Id_Type > >& ids_by_coarse,
   const map< string, pair< vector< Regular_Expression* >, vector< string > > >& keys,
   const Block_Backend< Tag_Index_Local, Id_Type >& items_db,
   typename Block_Backend< Tag_Index_Local, Id_Type >::Range_Iterator& tag_it,
   uint32 coarse_index)
{
  vector< Id_Type > new_ids = ids_by_coarse[coarse_index & 0x7fffff00];
  
  filter_ids_by_ntags(keys, items_db, tag_it, coarse_index & 0x7fffff00, new_ids);

  new_ids.swap(ids_by_coarse[coarse_index & 0x7fffff00]);
    
  filter_ids_by_ntags(keys, items_db, tag_it, coarse_index | 0x80000000, new_ids);

  vector< Id_Type > old_ids;
  old_ids.swap(ids_by_coarse[coarse_index & 0x7fffff00]);
  set_intersection(old_ids.begin(), old_ids.end(), new_ids.begin(), new_ids.end(),
      back_inserter(ids_by_coarse[coarse_index & 0x7fffff00]));
}


template< class TIndex, class TObject >
void Query_Statement::filter_by_tags
    (map< TIndex, vector< TObject > >& items,
     const File_Properties& file_prop, Resource_Manager& rman, Transaction& transaction)
{  
  // generate set of relevant coarse indices
  set< TIndex > coarse_indices;
  map< uint32, vector< typename TObject::Id_Type > > ids_by_coarse;
  generate_ids_by_coarse(coarse_indices, ids_by_coarse, items);
  
  // formulate range query
  set< pair< Tag_Index_Local, Tag_Index_Local > > range_set;
  formulate_range_query(range_set, coarse_indices);

  // prepare straight keys
  map< string, vector< Regular_Expression* > > key_union;
  for (vector< string >::const_iterator it = keys.begin(); it != keys.end();
      ++it)
    key_union[*it];
  for (vector< pair< string, Regular_Expression* > >::const_iterator
      it = key_regexes.begin(); it != key_regexes.end(); ++it)
    key_union[it->first].push_back(it->second);
  
  // iterate over the result
  map< TIndex, vector< TObject > > result;
  uint coarse_count = 0;
  Block_Backend< Tag_Index_Local, typename TObject::Id_Type > items_db
      (transaction.data_index(&file_prop));
  typename Block_Backend< Tag_Index_Local, typename TObject::Id_Type >::Range_Iterator
    tag_it(items_db.range_begin
    (Default_Range_Iterator< Tag_Index_Local >(range_set.begin()),
     Default_Range_Iterator< Tag_Index_Local >(range_set.end())));
  typename map< TIndex, vector< TObject > >::const_iterator
      item_it(items.begin());
  for (typename set< TIndex >::const_iterator it = coarse_indices.begin(); it != coarse_indices.end(); ++it)
  {
    if (++coarse_count >= 1024)
    {
      coarse_count = 0;
      rman.health_check(*this);
    }
    
    sort(ids_by_coarse[it->val()].begin(), ids_by_coarse[it->val()].end());
    
    filter_ids_by_tags(ids_by_coarse, key_union, items_db, tag_it, it->val());
    
    while ((item_it != items.end()) &&
        ((item_it->first.val() & 0x7fffff00) == it->val()))
    {
      for (typename vector< TObject >::const_iterator eit = item_it->second.begin();
	   eit != item_it->second.end(); ++eit)
      {
        if (binary_search(ids_by_coarse[it->val()].begin(),
            ids_by_coarse[it->val()].end(), eit->id))
	  result[item_it->first.val()].push_back(*eit);
      }
      ++item_it;
    }
  }
    
  items.swap(result);

  if (key_nregexes.empty() && key_nvalues.empty())
    return;
  
  // prepare negated keys
  map< string, pair< vector< Regular_Expression* >, vector< string > > > nkey_union;
  for (vector< pair< string, Regular_Expression* > >::const_iterator
      it = key_nregexes.begin(); it != key_nregexes.end(); ++it)
    nkey_union[it->first].first.push_back(it->second);
  for (vector< pair< string, string > >::const_iterator
      it = key_nvalues.begin(); it != key_nvalues.end(); ++it)
    nkey_union[it->first].second.push_back(it->second);
  
  // iterate over the result
  result.clear();
  coarse_count = 0;
  typename Block_Backend< Tag_Index_Local, typename TObject::Id_Type >::Range_Iterator
      ntag_it(items_db.range_begin
      (Default_Range_Iterator< Tag_Index_Local >(range_set.begin()),
       Default_Range_Iterator< Tag_Index_Local >(range_set.end())));
  item_it = items.begin();
  for (typename set< TIndex >::const_iterator it = coarse_indices.begin(); it != coarse_indices.end(); ++it)
  {
    if (++coarse_count >= 1024)
    {
      coarse_count = 0;
      rman.health_check(*this);
    }
    
    sort(ids_by_coarse[it->val()].begin(), ids_by_coarse[it->val()].end());
    
    filter_ids_by_ntags(ids_by_coarse, nkey_union, items_db, ntag_it, it->val());
    
    while ((item_it != items.end()) &&
        ((item_it->first.val() & 0x7fffff00) == it->val()))
    {
      for (typename vector< TObject >::const_iterator eit = item_it->second.begin();
           eit != item_it->second.end(); ++eit)
      {
        if (binary_search(ids_by_coarse[it->val()].begin(),
            ids_by_coarse[it->val()].end(), eit->id))
          result[item_it->first.val()].push_back(*eit);
      }
      ++item_it;
    }
  }
  
  items.swap(result);
}


template< class Id_Type >
void Query_Statement::progress_1(vector< Id_Type >& ids, bool& invert_ids, uint64 timestamp,
                                 Answer_State& answer_state, bool check_keys_late,
                                 const File_Properties& file_prop, const File_Properties& attic_file_prop,
                                 Resource_Manager& rman)
{
  if ((!keys.empty() && !check_keys_late)
     || !key_values.empty() || (!key_regexes.empty() && !check_keys_late))
  {
    collect_ids< Id_Type >(file_prop, attic_file_prop, rman, timestamp, check_keys_late).swap(ids);
    if (!key_nvalues.empty() || !key_nregexes.empty())
    {
      vector< Id_Type > non_ids = collect_non_ids< Id_Type >(file_prop, attic_file_prop, rman, timestamp);
      vector< Id_Type > diff_ids(ids.size(), Id_Type());
      diff_ids.erase(set_difference(ids.begin(), ids.end(), non_ids.begin(), non_ids.end(),
		     diff_ids.begin()), diff_ids.end());
      ids.swap(diff_ids);
    }
    if (ids.empty())
      answer_state = data_collected;
  }
  else if ((!key_nvalues.empty() || !key_nregexes.empty()) && !check_keys_late)
  {
    invert_ids = true;
    collect_non_ids< Id_Type >(file_prop, attic_file_prop, rman, timestamp).swap(ids);
  }
}


template< class Id_Type >
void Query_Statement::progress_1(vector< Id_Type >& ids, bool& invert_ids,
                                 Answer_State& answer_state, bool check_keys_late,
                                 const File_Properties& file_prop,
                                 Resource_Manager& rman)
{
  if ((!keys.empty() && !check_keys_late)
     || !key_values.empty() || (!key_regexes.empty() && !check_keys_late))
  {
    collect_ids< Id_Type >(file_prop, rman, check_keys_late).swap(ids);
    if (!key_nvalues.empty() || !key_nregexes.empty())
    {
      vector< Id_Type > non_ids = collect_non_ids< Id_Type >(file_prop, rman);
      vector< Id_Type > diff_ids(ids.size(), Id_Type());
      diff_ids.erase(set_difference(ids.begin(), ids.end(), non_ids.begin(), non_ids.end(),
                     diff_ids.begin()), diff_ids.end());
      ids.swap(diff_ids);
    }
    if (ids.empty())
      answer_state = data_collected;
  }
  else if ((!key_nvalues.empty() || !key_nregexes.empty()) && !check_keys_late)
  {
    invert_ids = true;
    collect_non_ids< Id_Type >(file_prop, rman).swap(ids);
  }
}


template< class Id_Type >
void Query_Statement::collect_nodes(vector< Id_Type >& ids,
				 bool& invert_ids, Answer_State& answer_state, Set& into,
				 Resource_Manager& rman)
{
  for (vector< Query_Constraint* >::iterator it = constraints.begin();
      it != constraints.end() && answer_state < data_collected; ++it)
  {
    if ((*it)->collect_nodes(rman, into, ids, invert_ids))
      answer_state = data_collected;
  }
}


template< class Id_Type >
void Query_Statement::collect_elems(vector< Id_Type >& ids,
				 bool& invert_ids, Answer_State& answer_state, Set& into,
				 Resource_Manager& rman)
{
  for (vector< Query_Constraint* >::iterator it = constraints.begin();
      it != constraints.end() && answer_state < data_collected; ++it)
  {
    if ((*it)->collect(rman, into, type, ids, invert_ids))
      answer_state = data_collected;
  }
}


void Query_Statement::execute(Resource_Manager& rman)
{
  Answer_State answer_state = nothing;
  Set into;
  uint64 timestamp = rman.get_desired_timestamp();
  if (timestamp == 0)
    timestamp = NOW;
  
  set_progress(1);
  rman.health_check(*this);

  bool check_keys_late = false;
  for (vector< Query_Constraint* >::iterator it = constraints.begin(); it != constraints.end(); ++it)
    check_keys_late |= (*it)->delivers_data();

  {
    vector< Node::Id_Type > node_ids;
    vector< Uint32_Index > ids;
    bool invert_ids = false;

    if (type == QUERY_NODE)
    {
      progress_1(node_ids, invert_ids, timestamp, answer_state, check_keys_late,
                 *osm_base_settings().NODE_TAGS_GLOBAL, *attic_settings().NODE_TAGS_GLOBAL, rman);
      collect_nodes(node_ids, invert_ids, answer_state, into, rman);
    }
    else if (type == QUERY_WAY)
    {
      progress_1(ids, invert_ids, timestamp, answer_state, check_keys_late,
                 *osm_base_settings().WAY_TAGS_GLOBAL, *attic_settings().WAY_TAGS_GLOBAL, rman);
      collect_elems(ids, invert_ids, answer_state, into, rman);
    }
    else if (type == QUERY_RELATION)
    {
      progress_1(ids, invert_ids, timestamp, answer_state, check_keys_late,
                 *osm_base_settings().RELATION_TAGS_GLOBAL,  *attic_settings().RELATION_TAGS_GLOBAL, rman);
      collect_elems(ids, invert_ids, answer_state, into, rman);
    }
    else if (type == QUERY_AREA)
    {
      progress_1(ids, invert_ids, answer_state,
		 check_keys_late, *area_settings().AREA_TAGS_GLOBAL, rman);
      collect_elems(ids, invert_ids, answer_state, into, rman);
    }
    
    set_progress(2);
    rman.health_check(*this);

    set< pair< Uint32_Index, Uint32_Index > > range_req_32;
    set< pair< Uint31_Index, Uint31_Index > > range_req_31;
    
    if (type == QUERY_NODE)
    {
      for (vector< Query_Constraint* >::iterator it = constraints.begin();
          it != constraints.end() && answer_state < ranges_collected; ++it)
      {
        if ((*it)->get_ranges(rman, range_req_32))
	  answer_state = ranges_collected;
      }
    }
    else if (type == QUERY_WAY || type == QUERY_RELATION)
    {
      for (vector< Query_Constraint* >::iterator it = constraints.begin();
          it != constraints.end() && answer_state < ranges_collected; ++it)
      {
	if ((*it)->get_ranges(rman, range_req_31))
	  answer_state = ranges_collected;
      }
    }
    
    set_progress(3);
    rman.health_check(*this);

    if (type == QUERY_NODE)
    {
      for (vector< Query_Constraint* >::iterator it = constraints.begin();
          it != constraints.end() && answer_state < data_collected; ++it)
      {
	if ((*it)->get_data(*this, rman, into, range_req_32, node_ids, invert_ids, timestamp))
	  answer_state = data_collected;
      }
    }
    else if (type == QUERY_WAY || type == QUERY_RELATION || type == QUERY_AREA)
    {
      for (vector< Query_Constraint* >::iterator it = constraints.begin();
          it != constraints.end() && answer_state < data_collected; ++it)
      {
	if ((*it)->get_data(*this, rman, into, range_req_31, type, ids, invert_ids, timestamp))
	  answer_state = data_collected;
      }
    }
  
    set_progress(4);
    rman.health_check(*this);
    
    if (type == QUERY_NODE)
    {
      if (answer_state < ranges_collected && !invert_ids)
        range_req_32 = get_ranges_by_id_from_db< Uint32_Index >
            (node_ids, rman, *osm_base_settings().NODES);
      if (answer_state < data_collected)
        ::get_elements_by_id_from_db< Uint32_Index, Node_Skeleton >
            (into.nodes, into.attic_nodes,
             node_ids, invert_ids, timestamp, range_req_32, *this, rman,
             *osm_base_settings().NODES, *attic_settings().NODES);
    }
    else if (type == QUERY_WAY)
    {
      if (answer_state < ranges_collected && !invert_ids)
	range_req_31 = get_ranges_by_id_from_db< Uint31_Index >
	    (ids, rman, *osm_base_settings().WAYS);
      if (answer_state < data_collected)
	::get_elements_by_id_from_db< Uint31_Index, Way_Skeleton >
	    (into.ways, into.attic_ways,
             ids, invert_ids, timestamp, range_req_31, *this, rman,
             *osm_base_settings().WAYS, *attic_settings().WAYS);
    }
    else if (type == QUERY_RELATION)
    {
      if (answer_state < ranges_collected && !invert_ids)
	range_req_31 = get_ranges_by_id_from_db< Uint31_Index >
	    (ids, rman, *osm_base_settings().RELATIONS);
      if (answer_state < data_collected)
	::get_elements_by_id_from_db< Uint31_Index, Relation_Skeleton >
	    (into.relations, into.attic_relations,
             ids, invert_ids, timestamp, range_req_31, *this, rman,
             *osm_base_settings().RELATIONS, *attic_settings().RELATIONS);
    }
    else if (type == QUERY_AREA)
    {
      if (answer_state < data_collected)
	get_elements_by_id_from_db(into.areas, ids, invert_ids, rman, *area_settings().AREAS);
    }
  }
  
  set_progress(5);
  rman.health_check(*this);

//   std::cout<<'\n';
//   for (map< Uint32_Index, vector< Node_Skeleton > >::const_iterator it = into.nodes.begin();
//        it != into.nodes.end(); ++it)
//   {
//     for (vector< Node_Skeleton >::const_iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2)
//       std::cout<<it2->id.val()<<'\n';
//   }
//   std::cout<<'\n';
//   for (map< Uint32_Index, vector< Attic< Node_Skeleton > > >::const_iterator
//        it = into.attic_nodes.begin(); it != into.attic_nodes.end(); ++it)
//   {
//     for (vector< Attic< Node_Skeleton > >::const_iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2)
//       std::cout<<it2->id.val()<<'\n';
//   }
//   std::cout<<'\n';
  
  for (vector< Query_Constraint* >::iterator it = constraints.begin();
      it != constraints.end(); ++it)
    (*it)->filter(rman, into, timestamp);
  
  set_progress(6);
  rman.health_check(*this);
  
  filter_attic_elements(rman, timestamp, into.nodes, into.attic_nodes);
  filter_attic_elements(rman, timestamp, into.ways, into.attic_ways);
  filter_attic_elements(rman, timestamp, into.relations, into.attic_relations);
  
  set_progress(7);
  rman.health_check(*this);
  
  if (check_keys_late)
  {
    filter_by_tags(into.nodes, *osm_base_settings().NODE_TAGS_LOCAL,
		   rman, *rman.get_transaction());
    filter_by_tags(into.ways, *osm_base_settings().WAY_TAGS_LOCAL,
		   rman, *rman.get_transaction());
    filter_by_tags(into.relations, *osm_base_settings().RELATION_TAGS_LOCAL,
		   rman, *rman.get_transaction());
    if (rman.get_area_transaction())
      filter_by_tags(into.areas, *area_settings().AREA_TAGS_LOCAL,
		     rman, *rman.get_transaction());
  }
  
  set_progress(8);
  rman.health_check(*this);
       
  for (vector< Query_Constraint* >::iterator it = constraints.begin();
      it != constraints.end(); ++it)
    (*it)->filter(*this, rman, into, timestamp);
    
  set_progress(9);
  rman.health_check(*this);
  
  clear_empty_indices(into.nodes);
  clear_empty_indices(into.attic_nodes);
  clear_empty_indices(into.ways);
  clear_empty_indices(into.attic_ways);
  clear_empty_indices(into.relations);
  clear_empty_indices(into.areas);

  transfer_output(rman, into);
  rman.health_check(*this);
}

//-----------------------------------------------------------------------------

Generic_Statement_Maker< Has_Kv_Statement > Has_Kv_Statement::statement_maker("has-kv");

Has_Kv_Statement::Has_Kv_Statement
    (int line_number_, const map< string, string >& input_attributes, Query_Constraint* bbox_limitation)
    : Statement(line_number_), regex(0), straight(true)
{
  map< string, string > attributes;
  
  attributes["k"] = "";
  attributes["v"] = "";
  attributes["regv"] = "";
  attributes["modv"] = "";
  attributes["case"] = "sensitive";
  
  eval_attributes_array(get_name(), attributes, input_attributes);
  
  key = attributes["k"];
  value = attributes["v"];
  
  if (key == "")
  {
    ostringstream temp("");
    temp<<"For the attribute \"k\" of the element \"has-kv\""
	<<" the only allowed values are non-empty strings.";
    add_static_error(temp.str());
  }
  
  bool case_sensitive = false;
  if (attributes["case"] != "ignore")
  {
    if (attributes["case"] != "sensitive")
      add_static_error("For the attribute \"case\" of the element \"has-kv\""
	  " the only allowed values are \"sensitive\" or \"ignore\".");
    case_sensitive = true;
  }
  
  if (attributes["regv"] != "")
  {
    if (value != "")
    {
      ostringstream temp("");
      temp<<"In the element \"has-kv\" only one of the attributes \"v\" and \"regv\""
            " can be nonempty.";
      add_static_error(temp.str());
    }
    
    try
    {
      regex = new Regular_Expression(attributes["regv"], case_sensitive);
    }
    catch (Regular_Expression_Error e)
    {
      add_static_error("Invalid regular expression: \"" + attributes["regv"] + "\"");
    }
  }
  
  if (attributes["modv"] == "" || attributes["modv"] == "not")
  {
    if (attributes["modv"] == "not")
      straight = false;
  }
  else
  {
    ostringstream temp("");
    temp<<"In the element \"has-kv\" the attribute \"modv\" can only be empty or set to \"not\".";
    add_static_error(temp.str());
  }
}

Has_Kv_Statement::~Has_Kv_Statement()
{
  if (regex)
    delete regex;
}
