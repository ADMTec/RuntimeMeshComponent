﻿// Copyright (c) 2015-2024 TriAxis Games, L.L.C. All Rights Reserved.

#include "Data/RealtimeMeshUpdateBuilder.h"
#include "RealtimeMeshComponentModule.h"

#define LOCTEXT_NAMESPACE "RealtimeMesh"

namespace RealtimeMesh
{
	FRealtimeMeshAccessContext::FRealtimeMeshAccessContext(const TSharedRef<const FRealtimeMesh>& InMesh)
		: ReadGuard(InMesh->GetSharedResources()->GetGuard())
		, Resources(InMesh->GetSharedResources())
	{ }

	FRealtimeMeshAccessContext::FRealtimeMeshAccessContext(const FRealtimeMeshSharedResourcesRef& InResources)
		: ReadGuard(InResources->GetGuard())
		, Resources(InResources)
	{ }

	FRealtimeMeshUpdateContext::FRealtimeMeshUpdateContext(const TSharedRef<FRealtimeMesh>& InMesh)
		: WriteGuard(InMesh->GetSharedResources()->GetGuard())
		, ProxyBuilder(!InMesh->GetRenderProxy().IsValid())
		, Resources(InMesh->GetSharedResources())
		, UpdateState(Resources->CreateUpdateState())
		, RHICmdList(InPlace)
	{ }

	FRealtimeMeshUpdateContext::FRealtimeMeshUpdateContext(const FRealtimeMeshSharedResourcesRef& InResources)
		: WriteGuard(InResources->GetGuard())
		, ProxyBuilder(!InResources->GetOwner().IsValid() || !InResources->GetOwner()->GetRenderProxy().IsValid())
		, Resources(InResources)
		, UpdateState(Resources->CreateUpdateState())
		, RHICmdList(InPlace)
	{ }

	FRealtimeMeshUpdateContext::~FRealtimeMeshUpdateContext()
	{
		if (RHICmdList.IsSet())
		{
			Commit();
		}
	}

	TFuture<ERealtimeMeshProxyUpdateStatus> FRealtimeMeshUpdateContext::Commit()
	{
		if (auto Mesh = Resources->GetOwner())
		{
			Mesh->FinalizeUpdate(*this);

			// Force async cmd list to fire before we potentially submit the proxy
			RHICmdList.Reset();
			return ProxyBuilder.Commit(Mesh.ToSharedRef());
		}

		// Go ahead and submit the cmd list
		RHICmdList.Reset();
		return MakeFulfilledPromise<ERealtimeMeshProxyUpdateStatus>(ERealtimeMeshProxyUpdateStatus::NoProxy).GetFuture();
	}

	TFuture<ERealtimeMeshProxyUpdateStatus> FRealtimeMeshUpdateBuilder::Commit(const TSharedRef<FRealtimeMesh>& Mesh)
	{
		FRealtimeMeshUpdateContext UpdateContext(Mesh);

		for (auto& Task : Tasks)
		{
			Task(UpdateContext, *Mesh);
		}

		return UpdateContext.Commit();		
	}

	void FRealtimeMeshUpdateBuilder::AddMeshTask(TUniqueFunction<void(FRealtimeMeshUpdateContext&, FRealtimeMesh&)>&& Function)
	{
		Tasks.Add(MoveTemp(Function));
	}

	void FRealtimeMeshUpdateBuilder::AddLODTask(const FRealtimeMeshLODKey& LODKey, TUniqueFunction<void(FRealtimeMeshUpdateContext&, FRealtimeMeshLOD&)>&& Function)
	{
		AddMeshTask([LODKey, Func = MoveTemp(Function)](FRealtimeMeshUpdateContext& UpdateContext, const FRealtimeMesh& Mesh)
		{
			const FRealtimeMeshLODPtr LOD = Mesh.GetLOD(UpdateContext, LODKey);

			if (ensure(LOD.IsValid()))
			{
				Func(UpdateContext, *LOD.Get());
			}
			else
			{
				UE_LOG(RealtimeMeshLog, Error, TEXT("Failed to find LOD %s"), *LODKey.ToString());

				FMessageLog("RealtimeMesh").Error(
				FText::Format(LOCTEXT("RealtimeMeshUpdate_LODTask", "RealtimeMeshUpdate_LODTask: Failed to find LOD {0}"),
					FText::FromString(LODKey.ToString())));				
			}
		});
	}

	void FRealtimeMeshUpdateBuilder::AddSectionGroupTask(const FRealtimeMeshSectionGroupKey& SectionGroupKey, TUniqueFunction<void(FRealtimeMeshUpdateContext&, FRealtimeMeshSectionGroup&)>&& Function)
	{
		AddLODTask(SectionGroupKey.LOD(), [SectionGroupKey, Func = MoveTemp(Function)](FRealtimeMeshUpdateContext& UpdateContext, const FRealtimeMeshLOD& LOD)
		{
			const FRealtimeMeshSectionGroupPtr SectionGroup = LOD.GetSectionGroup(UpdateContext, SectionGroupKey);

			if (ensure(SectionGroup.IsValid()))
			{
				Func(UpdateContext, *SectionGroup.Get());
			}
			else
			{
				UE_LOG(RealtimeMeshLog, Error, TEXT("Failed to find SectionGroup %s"), *SectionGroupKey.ToString());

				FMessageLog("RealtimeMesh").Error(
				FText::Format(LOCTEXT("RealtimeMeshUpdate_SectionGroupTask", "RealtimeMeshUpdate_SectionGroupTask: Failed to find SectionGroup {0}"),
					FText::FromString(SectionGroupKey.ToString())));				
			}
		});
	}

	void FRealtimeMeshUpdateBuilder::AddSectionTask(const FRealtimeMeshSectionKey& SectionKey, TUniqueFunction<void(FRealtimeMeshUpdateContext&, FRealtimeMeshSection&)>&& Function)
	{
		AddSectionGroupTask(SectionKey.SectionGroup(), [SectionKey, Func = MoveTemp(Function)](FRealtimeMeshUpdateContext& UpdateContext, const FRealtimeMeshSectionGroup& SectionGroup)
		{
			const FRealtimeMeshSectionPtr Section = SectionGroup.GetSection(UpdateContext, SectionKey);

			if (ensure(Section.IsValid()))
			{
				Func(UpdateContext, *Section.Get());
			}
			else
			{
				UE_LOG(RealtimeMeshLog, Error, TEXT("Failed to find Section %s"), *SectionKey.ToString());

				FMessageLog("RealtimeMesh").Error(
				FText::Format(LOCTEXT("RealtimeMeshUpdate_Section", "RealtimeMeshUpdate_Section: Failed to find Section {0}"),
					FText::FromString(SectionKey.ToString())));	
			}
		});
	}









	void FRealtimeMeshAccessor::Execute(const TSharedRef<const FRealtimeMesh>& Mesh)
	{
		FRealtimeMeshAccessContext LockContext(Mesh);
		
		for (auto& Task : Tasks)
		{
			Task(LockContext, *Mesh);
		}	
	}

	void FRealtimeMeshAccessor::AddMeshTask(TUniqueFunction<void(const FRealtimeMeshAccessContext&, const FRealtimeMesh&)>&& Function)
	{
		Tasks.Add(MoveTemp(Function));
	}

	void FRealtimeMeshAccessor::AddLODTask(const FRealtimeMeshLODKey& LODKey, TUniqueFunction<void(const FRealtimeMeshAccessContext&, const FRealtimeMeshLOD&)>&& Function)
	{
		AddMeshTask([LODKey, Func = MoveTemp(Function)](const FRealtimeMeshAccessContext& LockContext, const FRealtimeMesh& Mesh)
		{
			const FRealtimeMeshLODPtr LOD = Mesh.GetLOD(LockContext, LODKey);

			if (ensure(LOD.IsValid()))
			{
				Func(LockContext, *LOD.Get());
			}
			else
			{
				UE_LOG(RealtimeMeshLog, Error, TEXT("Failed to find LOD %s"), *LODKey.ToString());

				FMessageLog("RealtimeMesh").Error(
				FText::Format(LOCTEXT("RealtimeMeshAccessor_LODTask", "RealtimeMeshAccessor_LODTask: Failed to find LOD {0}"),
					FText::FromString(LODKey.ToString())));				
			}
		});
	}

	void FRealtimeMeshAccessor::AddSectionGroupTask(const FRealtimeMeshSectionGroupKey& SectionGroupKey, TUniqueFunction<void(const FRealtimeMeshAccessContext&, const FRealtimeMeshSectionGroup&)>&& Function)
	{
		AddLODTask(SectionGroupKey.LOD(), [SectionGroupKey, Func = MoveTemp(Function)](const FRealtimeMeshAccessContext& LockContext, const FRealtimeMeshLOD& LOD)
		{
			const FRealtimeMeshSectionGroupPtr SectionGroup = LOD.GetSectionGroup(LockContext, SectionGroupKey);

			if (ensure(SectionGroup.IsValid()))
			{
				Func(LockContext, *SectionGroup.Get());
			}
			else
			{
				UE_LOG(RealtimeMeshLog, Error, TEXT("Failed to find SectionGroup %s"), *SectionGroupKey.ToString());

				FMessageLog("RealtimeMesh").Error(
				FText::Format(LOCTEXT("RealtimeMeshAccessor_SectionGroupTask", "RealtimeMeshAccessor_SectionGroupTask: Failed to find SectionGroup {0}"),
					FText::FromString(SectionGroupKey.ToString())));				
			}
		});
	}

	void FRealtimeMeshAccessor::AddSectionTask(const FRealtimeMeshSectionKey& SectionKey, TUniqueFunction<void(const FRealtimeMeshAccessContext&, const FRealtimeMeshSection&)>&& Function)
	{
		AddSectionGroupTask(SectionKey.SectionGroup(), [SectionKey, Func = MoveTemp(Function)](const FRealtimeMeshAccessContext& LockContext, const FRealtimeMeshSectionGroup& SectionGroup)
		{
			const FRealtimeMeshSectionPtr Section = SectionGroup.GetSection(LockContext, SectionKey);

			if (ensure(Section.IsValid()))
			{
				Func(LockContext, *Section.Get());
			}
			else
			{
				UE_LOG(RealtimeMeshLog, Error, TEXT("Failed to find Section %s"), *SectionKey.ToString());

				FMessageLog("RealtimeMesh").Error(
				FText::Format(LOCTEXT("RealtimeMeshAccessor_Section", "RealtimeMeshAccessor_Section: Failed to find Section {0}"),
					FText::FromString(SectionKey.ToString())));	
			}
		});
	}
}
