// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/policy/cloud/cloud_policy_client_registration_helper.h"

#include <vector>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "base/values.h"
#include "google_apis/gaia/gaia_constants.h"
#include "google_apis/gaia/gaia_urls.h"
#include "google_apis/gaia/google_service_auth_error.h"
#include "google_apis/gaia/oauth2_token_service.h"

#if defined(OS_ANDROID)
#include "chrome/browser/signin/android_profile_oauth2_token_service.h"
#else
#include "google_apis/gaia/oauth2_access_token_consumer.h"
#include "google_apis/gaia/oauth2_access_token_fetcher.h"
#endif

namespace policy {

// OAuth2 scope for the userinfo service.
const char kServiceScopeGetUserInfo[] =
    "https://www.googleapis.com/auth/userinfo.email";

// The key under which the hosted-domain value is stored in the UserInfo
// response.
const char kGetHostedDomainKey[] = "hd";

typedef base::Callback<void(const std::string&)> StringCallback;

// This class fetches an OAuth2 token scoped for the userinfo and DM services.
// On Android, we use a special API to allow us to fetch a token for an account
// that is not yet logged in to allow fetching the token before the sign-in
// process is finished.
class CloudPolicyClientRegistrationHelper::TokenServiceHelper
    : public OAuth2TokenService::Consumer {
 public:
  TokenServiceHelper();

  void FetchAccessToken(
#if defined(OS_ANDROID)
      // TODO(atwilson): Remove this when StartRequestForUsername() is merged
      // into the base OAuth2TokenService class.
      AndroidProfileOAuth2TokenService* token_service,
#else
      OAuth2TokenService* token_service,
#endif
      const std::string& username,
      const StringCallback& callback);

 private:
  // OAuth2TokenService::Consumer implementation:
  virtual void OnGetTokenSuccess(const OAuth2TokenService::Request* request,
                                 const std::string& access_token,
                                 const base::Time& expiration_time) OVERRIDE;
  virtual void OnGetTokenFailure(const OAuth2TokenService::Request* request,
                                 const GoogleServiceAuthError& error) OVERRIDE;

  StringCallback callback_;
  scoped_ptr<OAuth2TokenService::Request> token_request_;
};

CloudPolicyClientRegistrationHelper::TokenServiceHelper::TokenServiceHelper() {}

void CloudPolicyClientRegistrationHelper::TokenServiceHelper::FetchAccessToken(
#if defined(OS_ANDROID)
    AndroidProfileOAuth2TokenService* token_service,
#else
    OAuth2TokenService* token_service,
#endif
    const std::string& username,
    const StringCallback& callback) {
  DCHECK(!token_request_);
  // Either the caller must supply a username, or the user must be signed in
  // already.
  DCHECK(!username.empty() || token_service->RefreshTokenIsAvailable());
  callback_ = callback;

  OAuth2TokenService::ScopeSet scopes;
  scopes.insert(GaiaConstants::kDeviceManagementServiceOAuth);
  scopes.insert(kServiceScopeGetUserInfo);

#if defined(OS_ANDROID)
  token_request_ =
      token_service->StartRequestForUsername(username, scopes, this);
#else
  token_request_ = token_service->StartRequest(scopes, this);
#endif
}

void CloudPolicyClientRegistrationHelper::TokenServiceHelper::OnGetTokenSuccess(
    const OAuth2TokenService::Request* request,
    const std::string& access_token,
    const base::Time& expiration_time) {
  DCHECK_EQ(token_request_.get(), request);
  callback_.Run(access_token);
}

void CloudPolicyClientRegistrationHelper::TokenServiceHelper::OnGetTokenFailure(
    const OAuth2TokenService::Request* request,
    const GoogleServiceAuthError& error) {
  DCHECK_EQ(token_request_.get(), request);
  callback_.Run("");
}

#if !defined(OS_ANDROID)
// This class fetches the OAuth2 token scoped for the userinfo and DM services.
// It uses an OAuth2AccessTokenFetcher to fetch it, given a login refresh token
// that can be used to authorize that request. This class is not needed on
// Android because we can use OAuth2TokenService to fetch tokens for accounts
// even before they are signed in.
class CloudPolicyClientRegistrationHelper::LoginTokenHelper
    : public OAuth2AccessTokenConsumer {
 public:
  LoginTokenHelper();

  void FetchAccessToken(const std::string& login_refresh_token,
                        net::URLRequestContextGetter* context,
                        const StringCallback& callback);

 private:
  // OAuth2AccessTokenConsumer implementation:
  virtual void OnGetTokenSuccess(const std::string& access_token,
                                 const base::Time& expiration_time) OVERRIDE;
  virtual void OnGetTokenFailure(
      const GoogleServiceAuthError& error) OVERRIDE;

  StringCallback callback_;
  scoped_ptr<OAuth2AccessTokenFetcher> oauth2_access_token_fetcher_;
};

CloudPolicyClientRegistrationHelper::LoginTokenHelper::LoginTokenHelper() {}

void CloudPolicyClientRegistrationHelper::LoginTokenHelper::FetchAccessToken(
    const std::string& login_refresh_token,
    net::URLRequestContextGetter* context,
    const StringCallback& callback) {
  DCHECK(!oauth2_access_token_fetcher_);
  callback_ = callback;

  // Start fetching an OAuth2 access token for the device management and
  // userinfo services.
  oauth2_access_token_fetcher_.reset(
      new OAuth2AccessTokenFetcher(this, context));
  std::vector<std::string> scopes;
  scopes.push_back(GaiaConstants::kDeviceManagementServiceOAuth);
  scopes.push_back(kServiceScopeGetUserInfo);
  GaiaUrls* gaia_urls = GaiaUrls::GetInstance();
  oauth2_access_token_fetcher_->Start(
      gaia_urls->oauth2_chrome_client_id(),
      gaia_urls->oauth2_chrome_client_secret(),
      login_refresh_token,
      scopes);
}

void CloudPolicyClientRegistrationHelper::LoginTokenHelper::OnGetTokenSuccess(
    const std::string& access_token,
    const base::Time& expiration_time) {
  callback_.Run(access_token);
}

void CloudPolicyClientRegistrationHelper::LoginTokenHelper::OnGetTokenFailure(
    const GoogleServiceAuthError& error) {
  callback_.Run("");
}

#endif

CloudPolicyClientRegistrationHelper::CloudPolicyClientRegistrationHelper(
    net::URLRequestContextGetter* context,
    CloudPolicyClient* client,
    bool should_force_load_policy,
    enterprise_management::DeviceRegisterRequest::Type registration_type)
    : context_(context),
      client_(client),
      should_force_load_policy_(should_force_load_policy),
      registration_type_(registration_type) {
  DCHECK(context_);
  DCHECK(client_);
}

CloudPolicyClientRegistrationHelper::~CloudPolicyClientRegistrationHelper() {
  // Clean up any pending observers in case the browser is shutdown while
  // trying to register for policy.
  if (client_)
    client_->RemoveObserver(this);
}


void CloudPolicyClientRegistrationHelper::StartRegistration(
#if defined(OS_ANDROID)
    AndroidProfileOAuth2TokenService* token_service,
#else
    OAuth2TokenService* token_service,
#endif
    const std::string& username,
    const base::Closure& callback) {
  DVLOG(1) << "Starting registration process with username";
  DCHECK(!client_->is_registered());
  callback_ = callback;
  client_->AddObserver(this);

  token_service_helper_.reset(new TokenServiceHelper());
  token_service_helper_->FetchAccessToken(
      token_service,
      username,
      base::Bind(&CloudPolicyClientRegistrationHelper::OnTokenFetched,
                 base::Unretained(this)));
}

#if !defined(OS_ANDROID)
void CloudPolicyClientRegistrationHelper::StartRegistrationWithLoginToken(
    const std::string& login_refresh_token,
    const base::Closure& callback) {
  DVLOG(1) << "Starting registration process with login token";
  DCHECK(!client_->is_registered());
  callback_ = callback;
  client_->AddObserver(this);

  login_token_helper_.reset(
      new CloudPolicyClientRegistrationHelper::LoginTokenHelper());
  login_token_helper_->FetchAccessToken(
      login_refresh_token,
      context_,
      base::Bind(&CloudPolicyClientRegistrationHelper::OnTokenFetched,
                 base::Unretained(this)));
}
#endif

void CloudPolicyClientRegistrationHelper::OnTokenFetched(
    const std::string& access_token) {
#if !defined(OS_ANDROID)
  login_token_helper_.reset();
#endif
  token_service_helper_.reset();

  if (access_token.empty()) {
    DLOG(WARNING) << "Could not fetch access token for "
                  << GaiaConstants::kDeviceManagementServiceOAuth;
    RequestCompleted();
    return;
  }

  // Cache the access token to be used after the GetUserInfo call.
  oauth_access_token_ = access_token;
  DVLOG(1) << "Fetched new scoped OAuth token:" << oauth_access_token_;
  // Now we've gotten our access token - contact GAIA to see if this is a
  // hosted domain.
  user_info_fetcher_.reset(new UserInfoFetcher(this, context_));
  user_info_fetcher_->Start(oauth_access_token_);
}

void CloudPolicyClientRegistrationHelper::OnGetUserInfoFailure(
    const GoogleServiceAuthError& error) {
  DVLOG(1) << "Failed to fetch user info from GAIA: " << error.state();
  user_info_fetcher_.reset();
  RequestCompleted();
}

void CloudPolicyClientRegistrationHelper::OnGetUserInfoSuccess(
    const base::DictionaryValue* data) {
  user_info_fetcher_.reset();
  if (!data->HasKey(kGetHostedDomainKey) && !should_force_load_policy_) {
    DVLOG(1) << "User not from a hosted domain - skipping registration";
    RequestCompleted();
    return;
  }
  DVLOG(1) << "Registering CloudPolicyClient for user from hosted domain";
  // The user is from a hosted domain, so it's OK to register the
  // CloudPolicyClient and make requests to DMServer.
  if (client_->is_registered()) {
    // Client should not be registered yet.
    NOTREACHED();
    RequestCompleted();
    return;
  }

  // Kick off registration of the CloudPolicyClient with our newly minted
  // oauth_access_token_.
  client_->Register(registration_type_, oauth_access_token_,
                    std::string(), false, std::string());
}

void CloudPolicyClientRegistrationHelper::OnPolicyFetched(
    CloudPolicyClient* client) {
  // Ignored.
}

void CloudPolicyClientRegistrationHelper::OnRegistrationStateChanged(
    CloudPolicyClient* client) {
  DVLOG(1) << "Client registration succeeded";
  DCHECK_EQ(client, client_);
  DCHECK(client->is_registered());
  RequestCompleted();
}

void CloudPolicyClientRegistrationHelper::OnClientError(
    CloudPolicyClient* client) {
  DVLOG(1) << "Client registration failed";
  DCHECK_EQ(client, client_);
  RequestCompleted();
}

void CloudPolicyClientRegistrationHelper::RequestCompleted() {
  if (client_) {
    client_->RemoveObserver(this);
    // |client_| may be freed by the callback so clear it now.
    client_ = NULL;
    callback_.Run();
  }
}

}  // namespace policy
