/*
 * haptic_object.h
 *
 *  Created on: Feb 19, 2010
 *      Author: dc
 */

#ifndef BARRETT_SYSTEMS_ABSTRACT_HAPTIC_OBJECT_H_
#define BARRETT_SYSTEMS_ABSTRACT_HAPTIC_OBJECT_H_


#include <barrett/detail/ca_macro.h>

#include <barrett/units.h>
#include <barrett/systems/abstract/system.h>
#include <barrett/systems/abstract/single_io.h>


namespace barrett {
namespace systems {


class HapticObject : public System, public SingleInput<units::CartesianPosition::type> {
// IO
public:		Output<double> depthOutput;
protected:	Output<double>::Value* depthOutputValue;
public:		Output<units::CartesianForce::type> directionOutput;
protected:	Output<units::CartesianForce::type>::Value* directionOutputValue;

public:
	HapticObject() :
		SingleInput<units::CartesianPosition::type>(this),
		depthOutput(this, &depthOutputValue),
		directionOutput(this, &directionOutputValue) {}
	virtual ~HapticObject() {}

private:
	DISALLOW_COPY_AND_ASSIGN(HapticObject);
};


}
}


#endif /* BARRETT_SYSTEMS_ABSTRACT_HAPTIC_OBJECT_H_ */
