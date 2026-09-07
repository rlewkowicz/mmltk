/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_AxisPhysicsMSDModel_h
#define mozilla_layers_AxisPhysicsMSDModel_h

#include "AxisPhysicsModel.h"

namespace mozilla {
namespace layers {

class AxisPhysicsMSDModel : public AxisPhysicsModel {
 public:
  AxisPhysicsMSDModel(double aInitialPosition, double aInitialDestination,
                      double aInitialVelocity, double aSpringConstant,
                      double aDampingRatio);

  virtual ~AxisPhysicsMSDModel();

  double GetDestination() const;

  void SetDestination(double aDestination);

  bool IsFinished(double aSmallestVisibleIncrement) const;

 protected:
  double Acceleration(const State& aState) override;

 private:
  double mDestination;

  double mSpringConstant;

  double mSpringConstantSqrtXTwo;

  double mDampingRatio;
};

}  
}  

#endif
