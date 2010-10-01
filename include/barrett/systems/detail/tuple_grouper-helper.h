/*
 * tuple_grouper-helper.h
 *
 *  Created on: Nov 16, 2009
 *      Author: dc
 */


#include <boost/static_assert.hpp>
#include <boost/tuple/tuple.hpp>

#include <barrett/systems/abstract/system.h>


namespace barrett {
namespace systems {


template<
	typename T0, typename T1, typename T2, typename T3, typename T4,
	typename T5, typename T6, typename T7, typename T8, typename T9>
class TupleGrouper;


namespace detail {


template<size_t N,
	typename T0, typename T1, typename T2, typename T3, typename T4,
	typename T5, typename T6, typename T7, typename T8, typename T9>
struct InputHolder :
		public InputHolder<N-1, T0, T1, T2, T3, T4, T5, T6, T7, T8, T9> {

	typedef InputHolder<N-1, T0, T1, T2, T3, T4, T5, T6, T7, T8, T9>
			inherited_type;
	typedef TupleGrouper<T0, T1, T2, T3, T4, T5, T6, T7, T8, T9> tg_type;
	typedef typename tg_type::tuple_type tuple_type;

	explicit InputHolder(tg_type* parent) :
		inherited_type(parent), input(parent) {}

	template<size_t Index>
	System::Input<typename boost::tuples::element<Index, tuple_type>::type>&
	getInput() {
		BOOST_STATIC_ASSERT(Index < N);
		return ( static_cast<InputHolder<  //NOLINT: lint doesn't know that these are templates
			Index+1, T0, T1, T2, T3, T4, T5, T6, T7, T8, T9>*>(this) )->input;
	}

	bool valuesDefined() {
		return input.valueDefined()  &&  inherited_type::valuesDefined();
	}

	tuple_type getValues() {
		collectValues();
		return this->values;
	}

	System::Input<typename boost::tuples::element<N-1, tuple_type>::type> input;

protected:
	void collectValues() {
		inherited_type::collectValues();
		boost::tuples::get<N-1>(this->values) = input.getValue();
	}
};

template<
	typename T0, typename T1, typename T2, typename T3, typename T4,
	typename T5, typename T6, typename T7, typename T8, typename T9>
struct InputHolder<1, T0, T1, T2, T3, T4, T5, T6, T7, T8, T9> {

	typedef TupleGrouper<T0, T1, T2, T3, T4, T5, T6, T7, T8, T9> tg_type;
	typedef typename tg_type::tuple_type tuple_type;

	explicit InputHolder(
			tg_type* parent) :
		input(parent), values() {}

	template<size_t InputIndex>
	System::Input<typename boost::tuples::element<InputIndex, tuple_type>::type >& getInput() {  //NOLINT: line length is for clarity
		return input;
	}

	bool valuesDefined() {
		return input.valueDefined();
	}

	tuple_type getValues() {
		return tuple_type(input.getValue());
	}

	System::Input<typename boost::tuples::element<0, tuple_type>::type> input;
	tuple_type values;

protected:
	void collectValues() {
		boost::tuples::get<0>(values) = input.getValue();
	}
};



}
}
}