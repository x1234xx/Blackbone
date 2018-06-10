#pragma once

#include "../../Asm/IAsmHelper.h"
#include "../Process.h"

#include <type_traits>

// TODO: Find more elegant way to deduce calling convention
//       than defining each one manually

namespace blackbone
{
template<typename R, typename... Args>
class RemoteFunctionBase
{
public:
    using ReturnType = std::conditional_t<std::is_same_v<R, void>, int, R>;

    struct CallArguments
    {
        CallArguments( const Args&... args )
            : arguments{ AsmVariant( args )... }
        { 
        }

        template<size_t... S>
        CallArguments( const std::tuple<Args...>& args, std::index_sequence<S...> )
            : arguments{ std::get<S>( args )... }
        {
        }

        CallArguments( const std::initializer_list<AsmVariant>& args )
            : arguments{ args }
        {
            // Since initializer_list can't be moved from, dataStruct types must be fixed
            for (auto& arg : arguments)
            {
                if (!arg.buf.empty())
                    arg.imm_val = reinterpret_cast<uintptr_t>(arg.buf.data());
            }
        }

        // Manually set argument to custom value
        void set( int pos, const AsmVariant& newVal )
        {
            if (arguments.size() > static_cast<size_t>(pos))
                arguments[pos] = newVal;
        }

        std::vector<AsmVariant> arguments;
    };

public:
    RemoteFunctionBase( Process& proc, ptr_t ptr, eCalligConvention conv )
        : _process( proc )
        , _ptr( ptr )
        , _conv( conv )
    {
        static_assert(
            (... && !std::is_reference_v<Args>),
            "Please replace reference type with pointer type in function type specification"
            );
    }

    call_result_t<ReturnType> Call( CallArguments& args, ThreadPtr contextThread = nullptr )
    {
        ReturnType result = {};
        uint64_t tmpResult = 0;
        NTSTATUS status = STATUS_SUCCESS;
        auto a = AsmFactory::GetAssembler( _process.core().isWow64() );

        // Ensure RPC environment exists
        auto mode = contextThread == _process.remote().getWorker() ? Worker_CreateNew : Worker_None;
        status = _process.remote().CreateRPCEnvironment( mode, contextThread != nullptr );
        if (!NT_SUCCESS( status ))
            return call_result_t<ReturnType>( result, status );

        // FPU check
        constexpr bool isFloat = std::is_same_v<ReturnType, float>;
        constexpr bool isDouble = std::is_same_v<ReturnType, double> || std::is_same_v<ReturnType, long double>;

        // Deduce return type
        eReturnType retType = rt_int32;

        if constexpr (isFloat)
            retType = rt_float;
        else if constexpr (isDouble)
            retType = rt_double;
        else if constexpr (sizeof( ReturnType ) == sizeof( uint64_t ))
            retType = rt_int64;
        else if constexpr (!std::is_reference_v<ReturnType> && sizeof( ReturnType ) > sizeof( uint64_t ))
            retType = rt_struct;

        _process.remote().PrepareCallAssembly( *a, _ptr, args.arguments, _conv, retType );

        // Choose execution thread
        if (!contextThread)
        {
            status = _process.remote().ExecInNewThread( (*a)->make(), (*a)->getCodeSize(), tmpResult );
        }
        else if (contextThread == _process.remote().getWorker())
        {
            status = _process.remote().ExecInWorkerThread( (*a)->make(), (*a)->getCodeSize(), tmpResult );
        }
        else
        {
            status = _process.remote().ExecInAnyThread( (*a)->make(), (*a)->getCodeSize(), tmpResult, contextThread );
        }

        // Get function return value
        if (!NT_SUCCESS( status ) || !NT_SUCCESS( status = _process.remote().GetCallResult( result ) ))
            return call_result_t<ReturnType>( result, status );

        // Update arguments
        for (auto& arg : args.arguments)
            if (arg.type == AsmVariant::dataPtr)
                _process.memory().Read( arg.new_imm_val, arg.size, reinterpret_cast<void*>(arg.imm_val) );

        return call_result_t<ReturnType>( result, STATUS_SUCCESS );
    }

private:
    Process& _process;
    ptr_t _ptr = 0;
    eCalligConvention _conv = cc_cdecl;
};

// Remote function pointer
template<typename Fn>
class RemoteFunction;

#define DECLPFN(CALL_OPT, CALL_DEF) \
template<typename R, typename... Args> \
class RemoteFunction < R( CALL_OPT*)(Args...) > : public RemoteFunctionBase<R, Args...> \
{ \
public: \
    RemoteFunction( Process& proc, ptr_t ptr ) \
        : RemoteFunctionBase( proc, ptr, CALL_DEF ) { } \
\
    RemoteFunction( Process& proc, R( *ptr )(Args...) ) \
        : RemoteFunctionBase( proc, reinterpret_cast<ptr_t>(ptr), CALL_DEF ) { } \
\
    call_result_t<ReturnType> Call( const Args&... args ) \
    { \
        CallArguments a( args... ); \
        return RemoteFunctionBase::Call( a ); \
    } \
\
    call_result_t<ReturnType> Call( const std::tuple<Args...>& args, ThreadPtr contextThread = nullptr ) \
    { \
        CallArguments a( args, std::index_sequence_for<Args...>() ); \
        return RemoteFunctionBase::Call( a, contextThread ); \
    } \
\
    call_result_t<ReturnType> Call( const std::initializer_list<AsmVariant>& args, ThreadPtr contextThread = nullptr ) \
    { \
        CallArguments a( args ); \
        return RemoteFunctionBase::Call( a, contextThread ); \
    } \
\
    call_result_t<ReturnType> Call( CallArguments& args, ThreadPtr contextThread = nullptr ) \
    { \
        return RemoteFunctionBase::Call( args, contextThread ); \
    } \
\
    call_result_t<ReturnType> operator()( const Args&... args ) \
    { \
        CallArguments a( args... ); \
        return RemoteFunctionBase::Call( a ); \
    } \
};

//
// Calling convention specialization
//
DECLPFN( __cdecl, cc_cdecl );

// Under AMD64 these will be same declarations as __cdecl, so compilation will fail.
#ifdef USE32
DECLPFN( __stdcall,  cc_stdcall  );
DECLPFN( __thiscall, cc_thiscall );
DECLPFN( __fastcall, cc_fastcall );
#endif

}