////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2017 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#include "catch.hpp"
#include "common.h"

#include "StorageEngineMock.h"

#if USE_ENTERPRISE
  #include "Enterprise/Ldap/LdapFeature.h"
#endif

#include "V8/v8-globals.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/LogicalView.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/OperationOptions.h"
#include "Utils/SingleCollectionTransaction.h"
#include "Aql/AqlFunctionFeature.h"
#include "Aql/OptimizerRulesFeature.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "IResearch/IResearchCommon.h"
#include "IResearch/IResearchFeature.h"
#include "IResearch/IResearchFilterFactory.h"
#include "IResearch/IResearchView.h"
#include "IResearch/IResearchAnalyzerFeature.h"
#include "Logger/Logger.h"
#include "Logger/LogTopic.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "RestServer/DatabasePathFeature.h"
#include "RestServer/ViewTypesFeature.h"
#include "RestServer/AqlFeature.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/QueryRegistryFeature.h"
#include "RestServer/SystemDatabaseFeature.h"
#include "RestServer/TraverserEngineRegistryFeature.h"
#include "Sharding/ShardingFeature.h"
#include "Basics/VelocyPackHelper.h"
#include "Aql/Ast.h"
#include "Aql/Query.h"
#include "3rdParty/iresearch/tests/tests_config.hpp"

#include "IResearch/VelocyPackHelper.h"
#include "analysis/analyzers.hpp"
#include "analysis/token_attributes.hpp"
#include "utils/utf8_path.hpp"

#include <velocypack/Iterator.h>

extern const char* ARGV0; // defined in main.cpp

namespace {

// -----------------------------------------------------------------------------
// --SECTION--                                                 setup / tear-down
// -----------------------------------------------------------------------------

struct IResearchQueryComplexBooleanSetup {
  StorageEngineMock engine;
  arangodb::application_features::ApplicationServer server;
  std::unique_ptr<TRI_vocbase_t> system;
  std::vector<std::pair<arangodb::application_features::ApplicationFeature*, bool>> features;

  IResearchQueryComplexBooleanSetup(): engine(server), server(nullptr, nullptr) {
    arangodb::EngineSelectorFeature::ENGINE = &engine;

    arangodb::tests::init(true);

    // suppress INFO {authentication} Authentication is turned on (system only), authentication for unix sockets is turned on
    arangodb::LogTopic::setLogLevel(arangodb::Logger::AUTHENTICATION.name(), arangodb::LogLevel::WARN);

    // suppress log messages since tests check error conditions
    arangodb::LogTopic::setLogLevel(arangodb::Logger::FIXME.name(), arangodb::LogLevel::ERR); // suppress WARNING DefaultCustomTypeHandler called
    arangodb::LogTopic::setLogLevel(arangodb::iresearch::TOPIC.name(), arangodb::LogLevel::FATAL);
    irs::logger::output_le(iresearch::logger::IRL_FATAL, stderr);

    // setup required application features
    features.emplace_back(new arangodb::ViewTypesFeature(server), true);
    features.emplace_back(new arangodb::AuthenticationFeature(server), true);
    features.emplace_back(new arangodb::DatabasePathFeature(server), false);
    features.emplace_back(new arangodb::DatabaseFeature(server), false);
    features.emplace_back(new arangodb::ShardingFeature(server), false); // 
    features.emplace_back(new arangodb::QueryRegistryFeature(server), false); // must be first
    arangodb::application_features::ApplicationServer::server->addFeature(features.back().first); // need QueryRegistryFeature feature to be added now in order to create the system database
    system = irs::memory::make_unique<TRI_vocbase_t>(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 0, TRI_VOC_SYSTEM_DATABASE);
    features.emplace_back(new arangodb::SystemDatabaseFeature(server, system.get()), false); // required for IResearchAnalyzerFeature
    features.emplace_back(new arangodb::TraverserEngineRegistryFeature(server), false); // must be before AqlFeature
    features.emplace_back(new arangodb::AqlFeature(server), true);
    features.emplace_back(new arangodb::aql::OptimizerRulesFeature(server), true);
    features.emplace_back(new arangodb::aql::AqlFunctionFeature(server), true); // required for IResearchAnalyzerFeature
    features.emplace_back(new arangodb::iresearch::IResearchAnalyzerFeature(server), true);
    features.emplace_back(new arangodb::iresearch::IResearchFeature(server), true);

    #if USE_ENTERPRISE
      features.emplace_back(new arangodb::LdapFeature(server), false); // required for AuthenticationFeature with USE_ENTERPRISE
    #endif

    for (auto& f : features) {
      arangodb::application_features::ApplicationServer::server->addFeature(f.first);
    }

    for (auto& f : features) {
      f.first->prepare();
    }

    for (auto& f : features) {
      if (f.second) {
        f.first->start();
      }
    }

    auto* analyzers = arangodb::application_features::ApplicationServer::lookupFeature<
      arangodb::iresearch::IResearchAnalyzerFeature
    >();

    analyzers->emplace(
      "test_analyzer",
      "TestAnalyzer",
      "abc",
      irs::flags{ irs::frequency::type(), irs::position::type() } // required for PHRASE
    ); // cache analyzer

    analyzers->emplace(
      "test_csv_analyzer",
      "TestDelimAnalyzer",
      ","
    ); // cache analyzer

    auto* dbPathFeature = arangodb::application_features::ApplicationServer::getFeature<arangodb::DatabasePathFeature>("DatabasePath");
    arangodb::tests::setDatabasePath(*dbPathFeature); // ensure test data is stored in a unique directory
  }

  ~IResearchQueryComplexBooleanSetup() {
    system.reset(); // destroy before reseting the 'ENGINE'
    arangodb::AqlFeature(server).stop(); // unset singleton instance
    arangodb::LogTopic::setLogLevel(arangodb::iresearch::TOPIC.name(), arangodb::LogLevel::DEFAULT);
    arangodb::LogTopic::setLogLevel(arangodb::Logger::FIXME.name(), arangodb::LogLevel::DEFAULT);
    arangodb::application_features::ApplicationServer::server = nullptr;
    arangodb::EngineSelectorFeature::ENGINE = nullptr;

    // destroy application features
    for (auto& f : features) {
      if (f.second) {
        f.first->stop();
      }
    }

    for (auto& f : features) {
      f.first->unprepare();
    }

    arangodb::LogTopic::setLogLevel(arangodb::Logger::AUTHENTICATION.name(), arangodb::LogLevel::DEFAULT);
  }
}; // IResearchQuerySetup

}

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief setup
////////////////////////////////////////////////////////////////////////////////

TEST_CASE("IResearchQueryTestComplexBoolean", "[iresearch][iresearch-query]") {
  IResearchQueryComplexBooleanSetup s;
  UNUSED(s);

  TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
  std::vector<arangodb::velocypack::Builder> insertedDocs;
  arangodb::LogicalView* view;

  // create collection0
  {
    auto createJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection0\" }");
    auto collection = vocbase.createCollection(createJson->slice());
    REQUIRE((nullptr != collection));

    std::vector<std::shared_ptr<arangodb::velocypack::Builder>> docs {
      arangodb::velocypack::Parser::fromJson("{ \"seq\": -6, \"value\": null }"),
      arangodb::velocypack::Parser::fromJson("{ \"seq\": -5, \"value\": true }"),
      arangodb::velocypack::Parser::fromJson("{ \"seq\": -4, \"value\": \"abc\" }"),
      arangodb::velocypack::Parser::fromJson("{ \"seq\": -3, \"value\": 3.14 }"),
      arangodb::velocypack::Parser::fromJson("{ \"seq\": -2, \"value\": [ 1, \"abc\" ] }"),
      arangodb::velocypack::Parser::fromJson("{ \"seq\": -1, \"value\": { \"a\": 7, \"b\": \"c\" } }"),
    };

    arangodb::OperationOptions options;
    options.returnNew = true;
    arangodb::SingleCollectionTransaction trx(
      arangodb::transaction::StandaloneContext::Create(vocbase),
      *collection,
      arangodb::AccessMode::Type::WRITE
    );
    CHECK((trx.begin().ok()));

    for (auto& entry: docs) {
      auto res = trx.insert(collection->name(), entry->slice(), options);
      CHECK((res.ok()));
      insertedDocs.emplace_back(res.slice().get("new"));
    }

    CHECK((trx.commit().ok()));
  }

  // create collection1
  {
    auto createJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection1\" }");
    auto collection = vocbase.createCollection(createJson->slice());
    REQUIRE((nullptr != collection));

    irs::utf8_path resource;
    resource/=irs::string_ref(IResearch_test_resource_dir);
    resource/=irs::string_ref("simple_sequential.json");

    auto builder = arangodb::basics::VelocyPackHelper::velocyPackFromFile(resource.utf8());
    auto slice = builder.slice();
    REQUIRE(slice.isArray());

    arangodb::OperationOptions options;
    options.returnNew = true;
    arangodb::SingleCollectionTransaction trx(
      arangodb::transaction::StandaloneContext::Create(vocbase),
      *collection,
      arangodb::AccessMode::Type::WRITE
    );
    CHECK((trx.begin().ok()));

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto res = trx.insert(collection->name(), itr.value(), options);
      CHECK((res.ok()));
      insertedDocs.emplace_back(res.slice().get("new"));
    }

    CHECK((trx.commit().ok()));
  }

  // create view
  {
    auto createJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
    auto logicalView = vocbase.createView(createJson->slice());
    REQUIRE((false == !logicalView));

    view = logicalView.get();
    auto* impl = dynamic_cast<arangodb::iresearch::IResearchView*>(view);
    REQUIRE((false == !impl));

    auto updateJson = arangodb::velocypack::Parser::fromJson(
      "{ \"links\": {"
        "\"testCollection0\": { \"includeAllFields\": true, \"nestListValues\": true, \"storeValues\":\"id\" },"
        "\"testCollection1\": { \"includeAllFields\": true, \"analyzers\": [ \"test_analyzer\", \"identity\" ], \"storeValues\":\"id\" }"
      "}}"
    );
    CHECK((impl->properties(updateJson->slice(), true).ok()));
    std::set<TRI_voc_cid_t> cids;
    impl->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
    CHECK((2 == cids.size()));
    CHECK((TRI_ERROR_NO_ERROR == arangodb::tests::executeQuery(vocbase, "FOR d IN testView SEARCH 1 ==1 OPTIONS { waitForSync: true } RETURN d").code)); // commit
  }

  // (A || B || C || !D)
  // (prefix || phrase || exists || !field)
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[0].slice(),
      insertedDocs[1].slice(),
      insertedDocs[2].slice(),
      insertedDocs[4].slice(),
      insertedDocs[5].slice(),
      insertedDocs[10].slice(),
      insertedDocs[11].slice(),
      insertedDocs[12].slice(),
      insertedDocs[14].slice(),
      insertedDocs[15].slice(),
      insertedDocs[16].slice(),
      insertedDocs[17].slice(),
      insertedDocs[18].slice(),
      insertedDocs[20].slice(),
      insertedDocs[21].slice(),
      insertedDocs[23].slice(),
      insertedDocs[25].slice(),
      insertedDocs[27].slice(),
      insertedDocs[28].slice(),
      insertedDocs[30].slice(),
      insertedDocs[32].slice(),
      insertedDocs[33].slice(),
      insertedDocs[34].slice(),
      insertedDocs[35].slice(),
      insertedDocs[7].slice(), // STARTS_WITH does not match, PHRASE matches
      insertedDocs[8].slice(), // STARTS_WITH does not match, PHRASE matches
      insertedDocs[13].slice(), // STARTS_WITH does not match, PHRASE matches
      insertedDocs[19].slice(), // STARTS_WITH does not match, PHRASE matches
      insertedDocs[22].slice(), // STARTS_WITH does not match, PHRASE matches
      insertedDocs[24].slice(), // STARTS_WITH does not match, PHRASE matches
      insertedDocs[29].slice(), // STARTS_WITH does not match, PHRASE matches
      insertedDocs[36].slice(), // STARTS_WITH matches (duplicate term), PHRASE does not match
      insertedDocs[37].slice(), // STARTS_WITH matches (duplicate term), PHRASE does not match
      insertedDocs[6].slice(), // STARTS_WITH matches (unique term), PHRASE does not match
      insertedDocs[9].slice(), // STARTS_WITH matches (unique term), PHRASE does not match
      insertedDocs[26].slice(), // STARTS_WITH matches (unique term), PHRASE does not match
      insertedDocs[31].slice(), // STARTS_WITH matches (unique term), PHRASE does not match
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN testView SEARCH STARTS_WITH(d.prefix, 'abc') || ANALYZER(PHRASE(d['duplicated'], 'z'), 'test_analyzer') || EXISTS(d.same) || d['value'] != 3.14 SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      REQUIRE((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // (A && B && !C)
  // field && prefix && !exists
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[36].slice(), // STARTS_WITH matches (duplicated term)
      insertedDocs[37].slice(), // STARTS_WITH matches (duplicated term)
      insertedDocs[26].slice(), // STARTS_WITH matches (unique term short)
      insertedDocs[31].slice(), // STARTS_WITH matches (unique term long)
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN testView SEARCH d.same == 'xyz' && STARTS_WITH(d['prefix'], 'abc') && NOT EXISTS(d.value) SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // (A && B) || (C && D)
  // (field && prefix) || (phrase && exists)
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[7].slice(), // PHRASE matches
      insertedDocs[8].slice(), // PHRASE matches
      insertedDocs[13].slice(), // PHRASE matches
      insertedDocs[19].slice(), // PHRASE matches
      insertedDocs[22].slice(), // PHRASE matches
      insertedDocs[36].slice(), // STARTS_WITH matches (duplicate term)
      insertedDocs[37].slice(), // STARTS_WITH matches (duplicate term)
      insertedDocs[6].slice(), // STARTS_WITH matches (unique term 4char)
      insertedDocs[9].slice(), // STARTS_WITH matches (unique term 5char)
      insertedDocs[26].slice(), // STARTS_WITH matches (unique term 3char)
      insertedDocs[31].slice(), // STARTS_WITH matches (unique term 7char)
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN testView SEARCH (d['same'] == 'xyz' && STARTS_WITH(d.prefix, 'abc')) || (ANALYZER(PHRASE(d['duplicated'], 'z'), 'test_analyzer') && EXISTS(d.value)) SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // (A && B) || (C && D)
  // (field && prefix) || (phrase && exists)
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[7].slice(), // PHRASE matches
      insertedDocs[8].slice(), // PHRASE matches
      insertedDocs[13].slice(), // PHRASE matches
      insertedDocs[19].slice(), // PHRASE matches
      insertedDocs[22].slice(), // PHRASE matches
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN testView SEARCH (d['same'] == 'xyz' && STARTS_WITH(d.prefix, 'abc')) || (ANALYZER(PHRASE(d['duplicated'], 'z'), 'test_analyzer') && EXISTS(d.value)) SORT BM25(d) ASC, TFIDF(d) DESC, d.seq limit 5 RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }

  // (A || B) && (C || D || E)
  // (field || exists) && (starts_with || phrase || range)
  {
    std::vector<arangodb::velocypack::Slice> expected = {
      insertedDocs[3].slice(),
      insertedDocs[4].slice(),
      insertedDocs[5].slice(),
      insertedDocs[10].slice(),
      insertedDocs[11].slice(),
      insertedDocs[12].slice(),
      insertedDocs[14].slice(),
      insertedDocs[15].slice(),
      insertedDocs[16].slice(),
      insertedDocs[17].slice(),
      insertedDocs[18].slice(),
      insertedDocs[20].slice(),
      insertedDocs[21].slice(),
      insertedDocs[23].slice(),
      insertedDocs[25].slice(),
      insertedDocs[27].slice(),
      insertedDocs[28].slice(),
      insertedDocs[30].slice(),
      insertedDocs[32].slice(),
      insertedDocs[33].slice(),
      insertedDocs[34].slice(),
      insertedDocs[35].slice(),
      insertedDocs[24].slice(), // STARTS_WITH does not match, PHRASE matches, EXISTS does not match
      insertedDocs[29].slice(), // STARTS_WITH does not match, PHRASE matches, EXISTS does not match
      insertedDocs[7].slice(), // STARTS_WITH does not match, PHRASE matches, EXISTS matches
      insertedDocs[8].slice(), // STARTS_WITH does not match, PHRASE matches, EXISTS matches
      insertedDocs[13].slice(), // STARTS_WITH does not match, PHRASE matches, EXISTS matches
      insertedDocs[19].slice(), // STARTS_WITH does not match, PHRASE matches, EXISTS matches
      insertedDocs[22].slice(), // STARTS_WITH does not match, PHRASE matches, EXISTS matches
      insertedDocs[36].slice(), // STARTS_WITH matches (duplicate term), PHRASE does not match, EXISTS does not match
      insertedDocs[37].slice(), // STARTS_WITH matches (duplicate term), PHRASE does not match, EXISTS does not match
      insertedDocs[26].slice(), // STARTS_WITH matches (unique term), PHRASE does not match, EXISTS does not match
      insertedDocs[31].slice(), // STARTS_WITH matches (unique term), PHRASE does not match, EXISTS does not match
      insertedDocs[6].slice(), // STARTS_WITH matches (unique term), PHRASE does not match, EXISTS matches
      insertedDocs[9].slice(), // STARTS_WITH matches (unique term), PHRASE does not match, EXISTS matches
    };
    auto result = arangodb::tests::executeQuery(
      vocbase,
      "FOR d IN testView SEARCH (d.same == 'xyz' || EXISTS(d['value'])) && (STARTS_WITH(d.prefix, 'abc') || ANALYZER(PHRASE(d['duplicated'], 'z'), 'test_analyzer') || d.seq >= -3) SORT BM25(d) ASC, TFIDF(d) DESC, d.seq RETURN d"
    );
    REQUIRE(TRI_ERROR_NO_ERROR == result.code);
    auto slice = result.result->slice();
    CHECK(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      CHECK((i < expected.size()));
      CHECK((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++], resolved, true)));
    }

    CHECK((i == expected.size()));
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------