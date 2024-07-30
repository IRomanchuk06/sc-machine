/*
 * This source file is part of an OSTIS project. For the latest info, see http://ostis.net
 * Distributed under the MIT License
 * (See accompanying file COPYING.MIT or copy at http://opensource.org/licenses/MIT)
 */

#include "sc_template.hpp"

#include <vector>
#include <unordered_map>
#include <iostream>

#include "sc_template_private.hpp"
#include "sc_memory.hpp"

#include "sc_struct.hpp"

class ScTemplateLoader
{
  friend class ScTemplate;

protected:
  ScTemplateLoader(ScMemoryContext & ctx, ScTemplateParams const & params)
    : m_context(ctx)
  {
  }

  ScTemplate::Result operator()(ScTemplate * inTemplate, ScAddr & outTemplateAddr)
  {
    outTemplateAddr = m_context.CreateNode(ScType::NodeConstStruct);
    ScStruct templateStruct(m_context, outTemplateAddr);

    std::unordered_map<std::string, ScAddr> itemNamesToTemplateElements;
    auto const & ResolveAddr = [this, &templateStruct, &inTemplate, &itemNamesToTemplateElements](
                                   ScTemplateItem const & item,
                                   ScAddr const & sourceAddr = ScAddr::Empty,
                                   ScAddr const & targetAddr = ScAddr::Empty) -> ScAddr
    {
      ScAddr itemAddr;

      if (item.HasName())
      {
        auto const & replacementItemsAddrsIt =
            inTemplate->m_templateItemsNamesToReplacementItemsAddrs.find(item.m_name);
        if (replacementItemsAddrsIt != inTemplate->m_templateItemsNamesToReplacementItemsAddrs.cend())
          itemAddr = replacementItemsAddrsIt->second;

        auto const & templateElementsIt = itemNamesToTemplateElements.find(item.m_name);
        if (templateElementsIt != itemNamesToTemplateElements.cend())
          itemAddr = templateElementsIt->second;
      }

      if (!itemAddr.IsValid())
      {
        if (item.IsAddr())
          itemAddr = item.m_addrValue;
        else if (item.IsType())
        {
          if (sourceAddr.IsValid() && targetAddr.IsValid())
            itemAddr = m_context.CreateEdge(item.m_typeValue, sourceAddr, targetAddr);
          else if (item.m_typeValue.IsLink())
            itemAddr = m_context.CreateLink(item.m_typeValue);
          else
            itemAddr = m_context.CreateNode(item.m_typeValue);
        }

        if (item.HasName())
          itemNamesToTemplateElements.insert({item.m_name, itemAddr});
      }

      templateStruct << itemAddr;

      return itemAddr;
    };

    for (ScTemplateTriple * triple : inTemplate->m_templateTriples)
    {
      auto const & values = triple->GetValues();

      ScTemplateItem const & sourceItem = values[0];
      ScTemplateItem const & targetItem = values[2];
      ScTemplateItem const & connectorItem = values[1];

      ScAddr const & sourceAddr = ResolveAddr(sourceItem);
      ScAddr const & targetAddr = ResolveAddr(targetItem);
      ResolveAddr(connectorItem, sourceAddr, targetAddr);
    }

    return ScTemplate::Result(true);
  }

protected:
  ScAddr m_templateAddr;
  ScMemoryContext & m_context;
  ScTemplateParams m_params;
};

ScTemplate::Result ScTemplate::ToScTemplate(
    ScMemoryContext & ctx,
    ScAddr & scTemplateAddr,
    ScTemplateParams const & params)
{
  ScTemplateLoader loader(ctx, params);
  return loader(this, scTemplateAddr);
}
