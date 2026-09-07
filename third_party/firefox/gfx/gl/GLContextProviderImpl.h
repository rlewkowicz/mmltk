/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(IN_GL_CONTEXT_PROVIDER_H)
#  error GLContextProviderImpl.h must only be included from GLContextProvider.h
#endif

#if !defined(GL_CONTEXT_PROVIDER_NAME)
#  error GL_CONTEXT_PROVIDER_NAME not defined
#endif

class GL_CONTEXT_PROVIDER_NAME {
 public:
  static already_AddRefed<GLContext> CreateForCompositorWidget(
      mozilla::widget::CompositorWidget* aCompositorWidget,
      bool aHardwareWebRender, bool aForceAccelerated);

  static already_AddRefed<GLContext> CreateHeadless(
      const GLContextCreateDesc&, nsACString* const out_failureId);

  static GLContext* GetGlobalContext();

  static void Shutdown();
};
