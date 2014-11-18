//
// SqratClassType: Type Translators
//

//
// Copyright (c) 2009 Brandon Jones
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
//    1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
//
//    2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
//
//    3. This notice may not be removed or altered from any source
//    distribution.
//

#if !defined(_SCRAT_CLASSTYPE_H_)
#define _SCRAT_CLASSTYPE_H_

#include <squirrel.h>
#include <map>

#include "sqratUtil.h"

namespace Sqrat
{

/// @cond DEV

// The copy function for a class
typedef SQInteger (*COPYFUNC)(HSQUIRRELVM, SQInteger, const void*);

// Every Squirrel class instance made by Sqrat has its type tag set to a AbstractStaticClassData object that is unique per C++ class
struct AbstractStaticClassData {
    AbstractStaticClassData() {}
    virtual ~AbstractStaticClassData() {}
    virtual SQUserPointer Cast(SQUserPointer ptr, SQUserPointer classType) = 0;
    AbstractStaticClassData* baseClass;
    string                   className;
    COPYFUNC                 copyFunc;
};

// StaticClassData keeps track of the nearest base class B and the class associated with itself C in order to cast C++ pointers to the right base class
template<class C, class B>
struct StaticClassData : public AbstractStaticClassData {
    virtual SQUserPointer Cast(SQUserPointer ptr, SQUserPointer classType) {
        if (classType != this) {
            ptr = baseClass->Cast(static_cast<B*>(static_cast<C*>(ptr)), classType);
        }
        return ptr;
    }
};

// Every Squirrel class object created by Sqrat in every VM has its own unique ClassData object stored in the registry table of the VM
template<class C>
struct ClassData {
    HSQOBJECT                          classObj;
    HSQOBJECT                          getTable;
    HSQOBJECT                          setTable;
    std::map<C*, HSQOBJECT>            instances;
    SharedPtr<AbstractStaticClassData> staticData;
};

// Internal helper class for managing classes
template<class C>
class ClassType {
private:

    static SQInteger instance_cleanup_hook(SQUserPointer ptr, SQInteger size) {
        SQUNUSED(size);
        std::pair<C*, std::map<C*, HSQOBJECT>*>* instance = reinterpret_cast<std::pair<C*, std::map<C*, HSQOBJECT>*>*>(ptr);
        instance->second->erase(instance->first);
        delete instance;
        return 0;
    }

public:

    static inline ClassData<C>* getClassData(HSQUIRRELVM vm) {
        sq_pushregistrytable(vm);
        sq_pushstring(vm, "__classes", -1);
#ifndef NDEBUG
        SQRESULT r = sq_rawget(vm, -2);
        assert(r == SQ_OK); // fails if getClassData is called when the data does not exist for the given VM yet
#else
        sq_rawget(vm, -2);
#endif
        sq_pushstring(vm, ClassName().c_str(), -1);
#ifndef NDEBUG
        r = sq_rawget(vm, -2);
        assert(r == SQ_OK); // fails if getClassData is called when the data does not exist for the given VM yet
#else
        sq_rawget(vm, -2);
#endif
        ClassData<C>** ud;
        sq_getuserdata(vm, -1, (SQUserPointer*)&ud, NULL);
        sq_pop(vm, 3);
        return *ud;
    }

    static inline WeakPtr<AbstractStaticClassData>& getStaticClassData() {
        static WeakPtr<AbstractStaticClassData> instance;
        return instance;
    }

    static inline bool hasClassData(HSQUIRRELVM vm) {
        if (!getStaticClassData().Expired()) {
            sq_pushregistrytable(vm);
            sq_pushstring(vm, "__classes", -1);
            if (SQ_SUCCEEDED(sq_rawget(vm, -2))) {
                sq_pushstring(vm, ClassName().c_str(), -1);
                if (SQ_SUCCEEDED(sq_rawget(vm, -2))) {
                    sq_pop(vm, 3);
                    return true;
                }
                sq_pop(vm, 1);
            }
            sq_pop(vm, 1);
        }
        return false;
    }

    static inline AbstractStaticClassData*& BaseClass() {
        assert(getStaticClassData().Expired() == false); // fails because called before the Sqrat::Class for this type exists
        return getStaticClassData().Lock()->baseClass;
    }

    static inline string& ClassName() {
        assert(getStaticClassData().Expired() == false); // fails because called before the Sqrat::Class for this type exists
        return getStaticClassData().Lock()->className;
    }

    static inline COPYFUNC& CopyFunc() {
        assert(getStaticClassData().Expired() == false); // fails because called before the Sqrat::Class for this type exists
        return getStaticClassData().Lock()->copyFunc;
    }

    static void PushInstance(HSQUIRRELVM vm, C* ptr) {
        if (!ptr) {
            sq_pushnull(vm);
            return;
        }

        ClassData<C>* cd = getClassData(vm);

        typename std::map<C*, HSQOBJECT>::iterator it = cd->instances.find(ptr);
        if (it != cd->instances.end()) {
            sq_pushobject(vm, it->second);
            return;
        }

        sq_pushobject(vm, cd->classObj);
        sq_createinstance(vm, -1);
        sq_remove(vm, -2);
        sq_setinstanceup(vm, -1, new std::pair<C*, std::map<C*, HSQOBJECT>*>(ptr, &(cd->instances)));
        sq_setreleasehook(vm, -1, &instance_cleanup_hook);
        sq_getstackobj(vm, -1, &cd->instances[ptr]);
    }

    static void PushInstanceCopy(HSQUIRRELVM vm, const C& value) {
        sq_pushobject(vm, ClassType<C>::getClassData(vm)->classObj);
        sq_createinstance(vm, -1);
        sq_remove(vm, -2);
        CopyFunc()(vm, -1, &value);
    }

    static C* GetInstance(HSQUIRRELVM vm, SQInteger idx, bool nullAllowed = false) {
        AbstractStaticClassData*                 classType = NULL;
        std::pair<C*, std::map<C*, HSQOBJECT>*>* instance  = NULL;
        if (hasClassData(vm)) /* type checking only done if the value has type data else it may be enum */
        {
            if (nullAllowed && sq_gettype(vm, idx) == OT_NULL) {
                return NULL;
            }

            classType = getStaticClassData().Lock().Get();

#if !defined (SCRAT_NO_ERROR_CHECKING)
            if (SQ_FAILED(sq_getinstanceup(vm, idx, (SQUserPointer*)&instance, classType))) {
                Error::Throw(vm, Sqrat::Error::FormatTypeError(vm, idx, ClassName()));
                return NULL;
            }
#else
            sq_getinstanceup(vm, idx, (SQUserPointer*)&instance, 0);
#endif
        }
        else /* value is likely of integral type like enums, cannot return a pointer */
        {
#if !defined (SCRAT_NO_ERROR_CHECKING)
            Error::Throw(vm, Sqrat::Error::FormatTypeError(vm, idx, _SC("unknown")));
#endif
            return NULL;
        }
        AbstractStaticClassData* actualType;
        sq_gettypetag(vm, idx, (SQUserPointer*)&actualType);
        if (actualType == NULL) {
            SQInteger top = sq_gettop(vm);
            sq_getclass(vm, idx);
            while (actualType == NULL) {
                sq_getbase(vm, -1);
                sq_gettypetag(vm, -1, (SQUserPointer*)&actualType);
            }
            sq_settop(vm, top);
        }
        if (classType != actualType) {
            return static_cast<C*>(actualType->Cast(instance->first, classType));
        }
        return static_cast<C*>(instance->first);
    }
};

/// @endcond

}

#endif
