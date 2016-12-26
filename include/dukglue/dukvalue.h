#pragma once

#include <duktape.h>
#include <stdint.h>
#include <string>

#include <assert.h>  // TODO decide error handling (exceptions?)

// A variant class for Duktape values.
// This class is not really dependant on the rest of dukglue, but the rest of dukglue is integrated to support it.
// Script objects are persisted by copying a reference to the object into an array in the heap stash.
// When we need to push a reference to the object, we just look up that reference in the stash.

// DukValues can be copied freely. We use reference counting behind the scenes to keep track of when we need
// to remove our reference from the heap stash. Memory for reference counting is only allocated once a DukValue
// is copied (either by copy constructor or operator=). std::move can be used if you are trying to avoid ref counting
// for some reason.

// One script object can have multiple, completely separate DukValues "pointing" to it - in this case, there will be
// multiple entries in the "ref array" that point to the same object. This will happen if the same script object is
// put on the stack and turned into a DukValue multiple times independently (copy-constructing/operator=-ing 
// DukValues will not do this!). This is okay, as we are only keeping track of these objects to prevent garbage
// collection (and access them later). This could be changed to use a map structure to look up one canonical entry per
// script object in the "ref array" (I guess it would be more like a ref map in this case), but this would require a map
// lookup every time we construct a DukValue. The performance difference probably isn't *that* noticeable (a good map
// would probably be amortized constant-time lookup), but I am guessing constructing many separate DukValues that point
// to the same script object isn't a very common thing.
class DukValue {
public:
	enum Type {
		//NONE = DUK_TYPE_NONE,
		UNDEFINED = DUK_TYPE_UNDEFINED,
		NULLREF = DUK_TYPE_NULL,
		BOOLEAN = DUK_TYPE_BOOLEAN,
		NUMBER = DUK_TYPE_NUMBER,
		STRING = DUK_TYPE_STRING,
		OBJECT = DUK_TYPE_OBJECT,
		//BUFFER = DUK_TYPE_BUFFER,
		POINTER = DUK_TYPE_POINTER,
		//LIGHTFUNC = DUK_TYPE_LIGHTFUNC
	};

	// default constructor just makes an undefined-type DukValue
	inline DukValue() : mContext(NULL), mType(UNDEFINED), mRefCount(NULL) {}

	virtual ~DukValue() {
		// release any references we have
		release_ref_count();
	}

	// move constructor
	inline DukValue(DukValue&& move) {
		mContext = move.mContext;
		mType = move.mType;
		mPOD = move.mPOD;
		mRefCount = move.mRefCount;

		if (mType == STRING)
			mString = std::move(move.mString);

		move.mType = UNDEFINED;
		move.mRefCount = NULL;
	}

	inline DukValue& operator=(const DukValue& rhs) {
		// free whatever we had
		release_ref_count();

		// copy things
		mContext = rhs.mContext;
		mType = rhs.mType;
		mPOD = rhs.mPOD;

		if (mType == STRING)
			mString = rhs.mString;

		if (mType == OBJECT)
		{
			// ref counting increment
			if (rhs.mRefCount == NULL) {
				// not ref counted before, need to allocate memory
				const_cast<DukValue&>(rhs).mRefCount = new int(2);
				mRefCount = rhs.mRefCount;
			} else {
				// already refcounting, just increment
				mRefCount = rhs.mRefCount;
				*mRefCount = *mRefCount + 1;
			}
		}

		return *this;
	}

	// copy constructor
	inline DukValue(const DukValue& copy) : DukValue() {
		*this = copy;
	}

	// equality operator
	inline bool operator==(const DukValue& rhs) const
	{
		if (mType != rhs.mType || mContext != rhs.mContext)
			return false;

		switch (mType) {
		case UNDEFINED:
		case NULLREF:
			return true;
		case BOOLEAN:
			return mPOD.boolean == rhs.mPOD.boolean;
		case NUMBER:
			return mPOD.number == rhs.mPOD.number;
		case STRING:
			return mString == rhs.mString;

		case OBJECT:
		{
			// this could be optimized to only push ref_array once...
			this->push();
			rhs.push();
			bool equal = duk_equals(mContext, -1, -2) ? true : false;
			duk_pop_2(mContext);
			return equal;
		}

		case POINTER:
			return mPOD.pointer == rhs.mPOD.pointer;

		default:
			assert(false);
			return false;
		}
	}

	inline bool operator!=(const DukValue& rhs) const {
		return !(*this == rhs);
	}

	// copies the object at idx on the stack into a new DukValue and returns it
	static DukValue copy_from_stack(duk_context* ctx, duk_idx_t idx = -1, duk_uint_t accept_mask = ~0x0) {
		if (!duk_check_type_mask(ctx, idx, accept_mask)) {
			assert(false);
		}

		DukValue value;
		value.mContext = ctx;
		value.mType = duk_get_type(ctx, idx);
		switch (value.mType) {
		case UNDEFINED:
			break;

		case NULLREF:
			value.mPOD.pointer = NULL;
			break;

		case BOOLEAN:
			value.mPOD.boolean = duk_require_boolean(ctx, idx) ? true : false;
			break;

		case NUMBER:
			value.mPOD.number = duk_require_number(ctx, idx);
			break;

		case STRING:
		{
			duk_size_t len;
			const char* data = duk_get_lstring(ctx, idx, &len);
			value.mString.assign(data, len);
			break;
		}

		case OBJECT:
			value.mPOD.ref_array_idx = stash_ref(ctx, idx);
			break;

		case POINTER:
			value.mPOD.pointer = duk_require_pointer(ctx, idx);
			break;
		}

		return std::move(value);
	}

	// same as above (copy_from_stack), but also removes the value we copied from the stack
	static DukValue take_from_stack(duk_context* ctx, duk_idx_t idx = -1, duk_uint_t accept_mask = ~0x0) {
		DukValue val = copy_from_stack(ctx, idx, accept_mask);
		duk_remove(ctx, idx);
		return std::move(val);
	}

	// push the value we hold onto the stack
	inline void push() const {
		duk_context* ctx = mContext;

		switch (mType) {
		case UNDEFINED:
			duk_push_undefined(ctx);
			break;
		case NULLREF:
			duk_push_null(ctx);
			break;

		case BOOLEAN:
			duk_push_boolean(ctx, mPOD.boolean);
			break;

		case NUMBER:
			duk_push_number(ctx, mPOD.number);
			break;

		case STRING:
			duk_push_lstring(ctx, mString.data(), mString.size());
			break;

		case OBJECT:
			push_ref_array(ctx);
			duk_get_prop_index(ctx, -1, mPOD.ref_array_idx);
			duk_remove(ctx, -2);
			break;

		case POINTER:
			duk_push_pointer(ctx, mPOD.pointer);
			break;
		}
	}

	// various (type-safe) getters
	inline double as_double() const {
		if (mType != NUMBER)
			assert(false);
		return mPOD.number;
	}

	inline float as_float() const {
		if (mType != NUMBER)
			assert(false);
		return static_cast<float>(mPOD.number);
	}

	inline duk_int_t as_int() const {
		if (mType != NUMBER)
			assert(false);
		return static_cast<uint32_t>(mPOD.number);
	}

	inline duk_uint_t as_uint() const {
		if (mType != NUMBER)
			assert(false);
		return static_cast<uint32_t>(mPOD.number);
	}

	inline void* as_pointer() const {
		if (mType != POINTER && mType != NULLREF)
			assert(false);
		return mPOD.pointer;
	}

	inline const std::string& as_string() const {
		if (mType != STRING)
			assert(false);
		return mString;
	}

	inline const char* as_c_string() const {
		if (mType != STRING)
			assert(false);
		return mString.data();
	}

	inline Type type() const {
		return (Type) mType;
	}

	inline duk_context* context() const {
		return mContext;
	}

private:
	// THIS IS COMPLETELY UNRELATED TO DETAIL_REFS.H.
	// detail_refs.h stores a mapping of native object -> script object.
	// This just stores arbitrary script objects (which likely have no native object backing them).
	// If I was smarter I might merge the two implementations, but this one is simpler
	// (since we don't need the std::map here).
	static void push_ref_array(duk_context* ctx)
	{
		static const char* DUKVALUE_REF_ARRAY = "dukglue_dukvalue_refs";
		duk_push_heap_stash(ctx);

		if (!duk_has_prop_string(ctx, -1, DUKVALUE_REF_ARRAY)) {
			duk_push_array(ctx);

			// ref_array[0] = 0 (initialize free list as empty)
			duk_push_int(ctx, 0);
			duk_put_prop_index(ctx, -2, 0);

			duk_put_prop_string(ctx, -2, DUKVALUE_REF_ARRAY);
		}

		duk_get_prop_string(ctx, -1, DUKVALUE_REF_ARRAY);
		duk_remove(ctx, -2); // pop heap stash
	}

	// put a new reference into the ref array and return its index in the array
	static duk_uint_t stash_ref(duk_context* ctx, duk_idx_t idx)
	{
		push_ref_array(ctx);

		// if idx is relative, we need to adjust it to deal with the array we just pushed
		if (idx < 0)
			idx--;

		// find next free index
		// free indices are kept in a linked list, starting at ref_array[0]
		duk_get_prop_index(ctx, -1, 0);
		duk_uarridx_t next_free_idx = duk_get_uint(ctx, -1);
		duk_pop(ctx);

		if (next_free_idx == 0) {
			// no free spots in the array, make a new one at arr.length
			next_free_idx = duk_get_length(ctx, -1);
		} else {
			// free spot found, need to remove it from the free list
			// ref_array[0] = ref_array[next_free_idx]
			duk_get_prop_index(ctx, -1, next_free_idx);
			duk_put_prop_index(ctx, -2, 0);
		}

		duk_dup(ctx, idx);  // copy value we are storing (since store consumes it)
		duk_put_prop_index(ctx, -2, next_free_idx);  // store it (consumes duplicated value)
		duk_pop(ctx);  // pop ref array

		return next_free_idx;
	}

	// remove ref_array_idx from the ref array and add its spot to the free list (at refs[0])
	static void free_ref(duk_context* ctx, duk_uarridx_t ref_array_idx)
	{
		push_ref_array(ctx);

		// add this spot to the free list
		// refs[old_obj_idx] = refs[0] (implicitly gives up our reference)
		duk_get_prop_index(ctx, -1, 0);
		duk_put_prop_index(ctx, -2, ref_array_idx);

		// refs[0] = old_obj_idx
		duk_push_uint(ctx, ref_array_idx);
		duk_put_prop_index(ctx, -2, 0);

		duk_pop(ctx);  // pop ref array
	}

	// this is for reference counting - used to release our reference based on the state
	// of mRefCount. If mRefCount is NULL, we never got copy constructed, so we have ownership
	// of our reference and can free it. If it's not null and above 1, we decrement the counter
	// (someone else owns the reference). If it's not null and equal to 1, we are the last owner
	// of a previously shared reference, so we can free it.
	void release_ref_count()
	{
		if (mType == OBJECT)
		{
			if (mRefCount != NULL)
			{
				// sharing with another DukValue, are we the only one left?
				if (*mRefCount > 1) {  // still someone else referencing this
					*mRefCount = *mRefCount - 1;
				} else {
					// not sharing anymore, we can free it
					free_ref(mContext, mPOD.ref_array_idx);
					delete mRefCount;
				}

				mRefCount = NULL;
			} else {
				// not sharing with any other DukValue, free it
				free_ref(mContext, mPOD.ref_array_idx);
			}

			mType = UNDEFINED;
		}
	}

	duk_context* mContext;
	duk_int_t mType;  // our type - one of the standard Duktape DUK_TYPE_* values

	// This holds the plain-old-data types. Since this is a variant,
	// we hold only one value at a time, so this is a union to save
	// a bit of space.
	union ValueTypes {
		bool boolean;
		double number;
		void* pointer;
		duk_uarridx_t ref_array_idx;
	} mPOD;

	std::string mString;  // if it's a string, we store it with std::string
	int* mRefCount;  // if mType == OBJECT and we're sharing, this will point to our ref counter
};