#include "FiberFContext.h"

thread_local Fiber* sCurrentFiber = nullptr;
// Thor fix: fibers migrate between host threads; never let the compiler reuse a cached TLS base
[[gnu::noinline]] static Fiber*& _tlsCurrentFiber() { asm volatile("" ::: "memory"); return sCurrentFiber; }

using namespace boost::context::detail;

Fiber::Fiber(void (*fiberEntryPoint)(void* userParam), void* userParam, void* privateData)
	: m_entryPoint(fiberEntryPoint),
	  m_userParam(userParam),
	  m_privateData(privateData)
{
	const size_t stackSize = 2 * 1024 * 1024;
	m_stackPtr = malloc(stackSize);
	m_context = make_fcontext(static_cast<uint8*>(m_stackPtr) + stackSize, stackSize, Fiber::Start);
}

Fiber::Fiber(void* privateData) : m_privateData(privateData)
{
}

void Fiber::Start(transfer_t transfer)
{
	auto fiber = static_cast<Fiber*>(transfer.data);
	fiber->m_prevFiber->m_context = transfer.fctx;
	fiber->m_entryPoint(fiber->m_userParam);
}

Fiber::~Fiber()
{
	if (m_stackPtr)
		free(m_stackPtr);
}

Fiber* Fiber::PrepareCurrentThread(void* privateData)
{
	_tlsCurrentFiber() = new Fiber(privateData);
	return _tlsCurrentFiber();
}

void Fiber::Switch(Fiber& targetFiber)
{
	if (&targetFiber == _tlsCurrentFiber())
		return;

	Fiber* thisFiber = _tlsCurrentFiber();
	_tlsCurrentFiber() = &targetFiber;
	targetFiber.m_prevFiber = thisFiber;
	transfer_t transfer = jump_fcontext(targetFiber.m_context, &targetFiber);
	thisFiber->m_prevFiber->m_context = transfer.fctx;
}

void* Fiber::GetFiberPrivateData()
{
	return _tlsCurrentFiber()->m_privateData;
}
