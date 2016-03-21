////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_REST_HANDLER_REST_BASE_HANDLER_H
#define ARANGOD_REST_HANDLER_REST_BASE_HANDLER_H 1

#include "Basics/Common.h"
#include "HttpServer/HttpHandler.h"
#include "Rest/HttpResponse.h"

namespace arangodb {
namespace velocypack {
class Slice;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief default handler for error handling and json in-/output
////////////////////////////////////////////////////////////////////////////////

class RestBaseHandler : public rest::HttpHandler {
 public:
  explicit RestBaseHandler(rest::HttpRequest* request);

 public:
  void handleError(basics::Exception const&);

 public:

  //////////////////////////////////////////////////////////////////////////////
  /// @brief generates a result from VelocyPack
  //////////////////////////////////////////////////////////////////////////////

  virtual void generateResult(arangodb::velocypack::Slice const& slice);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief generates a result from VelocyPack
  //////////////////////////////////////////////////////////////////////////////

  virtual void generateResult(rest::HttpResponse::HttpResponseCode,
                              arangodb::velocypack::Slice const& slice);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief generates a cancel message
  //////////////////////////////////////////////////////////////////////////////

  virtual void generateCanceled();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief generates an error
  //////////////////////////////////////////////////////////////////////////////

  virtual void generateError(rest::HttpResponse::HttpResponseCode, int);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief generates an error
  //////////////////////////////////////////////////////////////////////////////

  virtual void generateError(rest::HttpResponse::HttpResponseCode, int,
                             std::string const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief generates an OUT_OF_MEMORY error
  //////////////////////////////////////////////////////////////////////////////

  virtual void generateOOMError();
};
}

#endif
