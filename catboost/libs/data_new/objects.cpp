#include "objects.h"
#include "util.h"

#include <catboost/libs/helpers/checksum.h>
#include <catboost/libs/helpers/parallel_tasks.h>
#include <catboost/libs/helpers/vector_helpers.h>

#include <util/generic/algorithm.h>
#include <util/generic/cast.h>
#include <util/stream/format.h>

#include <algorithm>


using namespace NCB;


void NCB::CheckGroupIds(
    ui32 objectCount,
    TMaybeData<TConstArrayRef<TGroupId>> groupIds,
    TMaybe<TObjectsGroupingPtr> objectsGrouping
) {
    if (!groupIds) {
        return;
    }
    auto groupIdsData = *groupIds;

    CheckDataSize(groupIdsData.size(), (size_t)objectCount, "group Ids", false);


    TVector<TGroupId> groupGroupIds;
    TGroupBounds currentGroupBounds(0); // used only if objectsGrouping is defined

    if (objectsGrouping.Defined()) {
        CheckDataSize(
            groupIdsData.size(),
            (size_t)(*objectsGrouping)->GetObjectCount(),
            "group Ids",
            false,
            "objectGrouping's object count",
            true
        );

        groupGroupIds.reserve((*objectsGrouping)->GetGroupCount());
        currentGroupBounds = (*objectsGrouping)->GetGroup(0);
    }

    TGroupId lastGroupId = groupIdsData[0];
    groupGroupIds.emplace_back(lastGroupId);

    // using ui32 for counters/indices here is safe because groupIdsData' size was checked above
    for (auto objectIdx : xrange(ui32(1), ui32(groupIdsData.size()))) {
        if (groupIdsData[objectIdx] != lastGroupId) {
            if (objectsGrouping.Defined()) {
                CB_ENSURE_INTERNAL(
                    objectIdx == currentGroupBounds.End,
                    "objectsGrouping and grouping by groupId have different ends for group #"
                    << (groupGroupIds.size() - 1)
                );
                currentGroupBounds = (*objectsGrouping)->GetGroup((ui32)groupGroupIds.size());
            }

            lastGroupId = groupIdsData[objectIdx];
            groupGroupIds.emplace_back(lastGroupId);
        }
    }

    Sort(groupGroupIds);
    auto it = std::adjacent_find(groupGroupIds.begin(), groupGroupIds.end());
    CB_ENSURE(it == groupGroupIds.end(), "group Ids are not consecutive");
}


TObjectsGrouping NCB::CreateObjectsGroupingFromGroupIds(
    ui32 objectCount,
    TMaybeData<TConstArrayRef<TGroupId>> groupIds
) {
    if (!groupIds) {
        return TObjectsGrouping(objectCount);
    }
    auto groupIdsData = *groupIds;

    CheckDataSize(groupIdsData.size(), (size_t)objectCount, "group Ids", false);

    TVector<TGroupBounds> groupBounds;
    {
        TVector<TGroupId> groupGroupIds;

        ui32 lastGroupBegin = 0;
        TGroupId lastGroupId = groupIdsData[0];
        groupGroupIds.emplace_back(lastGroupId);

        // using ui32 for counters/indices here is safe because groupIdsData' size was checked above
        for (auto objectIdx : xrange(ui32(1), ui32(groupIdsData.size()))) {
            if (groupIdsData[objectIdx] != lastGroupId) {
                lastGroupId = groupIdsData[objectIdx];
                groupGroupIds.emplace_back(lastGroupId);
                groupBounds.emplace_back(lastGroupBegin, objectIdx);
                lastGroupBegin = objectIdx;
            }
        }
        groupBounds.emplace_back(lastGroupBegin, ui32(groupIdsData.size()));

        // check that there're no groupId duplicates
        Sort(groupGroupIds);
        auto it = std::adjacent_find(groupGroupIds.begin(), groupGroupIds.end());
        CB_ENSURE(it == groupGroupIds.end(), "group Ids are not consecutive");
    }

    return TObjectsGrouping(std::move(groupBounds), true);
}


void NCB::TCommonObjectsData::PrepareForInitialization(
    const TDataMetaInfo& metaInfo,
    ui32 objectCount,
    ui32 prevTailCount
) {
    FeaturesLayout = metaInfo.FeaturesLayout;

    NCB::PrepareForInitialization(metaInfo.HasGroupId, objectCount, prevTailCount, &GroupIds);
    NCB::PrepareForInitialization(metaInfo.HasSubgroupIds, objectCount, prevTailCount, &SubgroupIds);
    NCB::PrepareForInitialization(metaInfo.HasTimestamp, objectCount, prevTailCount, &Timestamp);
}


void NCB::TCommonObjectsData::CheckAllExceptGroupIds() const {
    if (SubgroupIds) {
        CB_ENSURE(
            GroupIds,
            "non-empty SubgroupIds when GroupIds is not defined"
        );
        CheckDataSize(SubgroupIds->size(), GroupIds->size(), "Subgroup Ids", false, "Group Ids size");
    }
    if (Timestamp) {
        CheckDataSize(Timestamp->size(), (size_t)SubsetIndexing->Size(), "Timestamp");
    }
}

void NCB::TCommonObjectsData::Check(TMaybe<TObjectsGroupingPtr> objectsGrouping) const {
    CB_ENSURE_INTERNAL(FeaturesLayout, "FeaturesLayout is undefined");
    if (objectsGrouping.Defined()) {
        CheckDataSize(
            (*objectsGrouping)->GetObjectCount(),
            SubsetIndexing->Size(),
            "objectsGrouping's object count",
            false,
            "SubsetIndexing's Size"
        );
    }
    CheckGroupIds(SubsetIndexing->Size(), GroupIds, objectsGrouping);
    CheckAllExceptGroupIds();
}

NCB::TCommonObjectsData NCB::TCommonObjectsData::GetSubset(
    const TObjectsGroupingSubset& objectsGroupingSubset,
    NPar::TLocalExecutor* localExecutor
) const {
    TCommonObjectsData result;
    result.ResourceHolders = ResourceHolders;
    result.FeaturesLayout = FeaturesLayout;
    result.Order = Combine(Order, objectsGroupingSubset.GetObjectSubsetOrder());

    TVector<std::function<void()>> tasks;

    tasks.emplace_back(
        [&, this]() {
            result.SubsetIndexing = MakeAtomicShared<TArraySubsetIndexing<ui32>>(
                Compose(*SubsetIndexing, objectsGroupingSubset.GetObjectsIndexing())
            );
        }
    );

    tasks.emplace_back(
        [&, this]() {
            result.GroupIds = GetSubsetOfMaybeEmpty<TGroupId>(
                (TMaybeData<TConstArrayRef<TGroupId>>)GroupIds,
                objectsGroupingSubset.GetObjectsIndexing(),
                localExecutor
            );
        }
    );
    tasks.emplace_back(
        [&, this]() {
            result.SubgroupIds = GetSubsetOfMaybeEmpty<TSubgroupId>(
                (TMaybeData<TConstArrayRef<TSubgroupId>>)SubgroupIds,
                objectsGroupingSubset.GetObjectsIndexing(),
                localExecutor
            );
        }
    );
    tasks.emplace_back(
        [&, this]() {
            result.Timestamp = GetSubsetOfMaybeEmpty<ui64>(
                (TMaybeData<TConstArrayRef<ui64>>)Timestamp,
                objectsGroupingSubset.GetObjectsIndexing(),
                localExecutor
            );
        }
    );

    ExecuteTasksInParallel(&tasks, localExecutor);

    return result;
}

void NCB::TCommonObjectsData::Load(TFeaturesLayoutPtr featuresLayout, ui32 objectCount, IBinSaver* binSaver) {
    FeaturesLayout = featuresLayout;
    SubsetIndexing = MakeAtomicShared<TArraySubsetIndexing<ui32>>(TFullSubset<ui32>(objectCount));
    LoadMulti(binSaver, &Order, &GroupIds, &SubgroupIds, &Timestamp);
}

void NCB::TCommonObjectsData::SaveNonSharedPart(IBinSaver* binSaver) const {
    SaveMulti(binSaver, Order, GroupIds, SubgroupIds, Timestamp);
}


NCB::TObjectsDataProvider::TObjectsDataProvider(
    // if not defined - call CreateObjectsGroupingFromGroupIds
    TMaybe<TObjectsGroupingPtr> objectsGrouping,
    TCommonObjectsData&& commonData,
    bool skipCheck
) {
    if (objectsGrouping.Defined()) {
        if (!skipCheck) {
            commonData.Check(objectsGrouping);
        }
        ObjectsGrouping = std::move(*objectsGrouping);
    } else {
        if (!skipCheck) {
            commonData.CheckAllExceptGroupIds();
        }
        ObjectsGrouping = MakeIntrusive<TObjectsGrouping>(
            CreateObjectsGroupingFromGroupIds(
                commonData.SubsetIndexing->Size(),
                commonData.GroupIds
            )
        );
    }
    CommonData = std::move(commonData);

    if ((CommonData.Order == EObjectsOrder::Undefined) && CommonData.Timestamp) {
        const auto& timestamps = *CommonData.Timestamp;
        if ((ObjectsGrouping->GetObjectCount() > 1) &&
            std::is_sorted(timestamps.begin(), timestamps.end()) &&
            (timestamps.front() != timestamps.back()))
        {
            CommonData.Order = EObjectsOrder::Ordered;
        }
    }
}


void NCB::TRawObjectsData::PrepareForInitialization(const TDataMetaInfo& metaInfo) {
    // FloatFeatures and CatFeatures members are initialized at the end of building
    FloatFeatures.clear();
    FloatFeatures.resize((size_t)metaInfo.FeaturesLayout->GetFloatFeatureCount());

    CatFeatures.clear();
    const size_t catFeatureCount = (size_t)metaInfo.FeaturesLayout->GetCatFeatureCount();
    CatFeatures.resize(catFeatureCount);
    if (catFeatureCount) {
        if (!CatFeaturesHashToString) {
            CatFeaturesHashToString = MakeAtomicShared<TVector<THashMap<ui32, TString>>>();
        }
        CatFeaturesHashToString->resize(catFeatureCount);
    }
}


template <class TFeaturesColumn>
static void CheckDataSizes(
    ui32 objectCount,
    const TFeaturesLayout& featuresLayout,
    const EFeatureType featureType,
    const TVector<THolder<TFeaturesColumn>>& featuresData
) {
    CheckDataSize(
        featuresData.size(),
        (size_t)featuresLayout.GetFeatureCount(featureType),
        TStringBuilder() << "ObjectDataProvider's " << featureType << " features",
        false,
        TStringBuilder() << "featureLayout's " << featureType << " features size",
        true
    );

    for (auto featureIdx : xrange(featuresData.size())) {
        TString dataDescription =
            TStringBuilder() << "ObjectDataProvider's " << featureType << " feature #" << featureIdx;

        auto dataPtr = featuresData[featureIdx].Get();
        bool isAvailable = featuresLayout.GetInternalFeatureMetaInfo(featureIdx, featureType).IsAvailable;
        if (isAvailable) {
            CB_ENSURE_INTERNAL(
                dataPtr,
                dataDescription << " is null despite being available in featuresLayout"
            );
            CheckDataSize(
                dataPtr->GetSize(),
                objectCount,
                dataDescription,
                /*dataCanBeEmpty*/ false,
                "object count",
                /*internalCheck*/ true
            );
        }
    }
}


void NCB::TRawObjectsData::Check(
    ui32 objectCount,
    const TFeaturesLayout& featuresLayout,
    NPar::TLocalExecutor* localExecutor
) const {
    CheckDataSizes(objectCount, featuresLayout, EFeatureType::Float, FloatFeatures);

    if (CatFeatures.size()) {
        CheckDataSize(
            CatFeaturesHashToString->size(),
            CatFeatures.size(),
            "CatFeaturesHashToString",
            /*dataCanBeEmpty*/ false,
            "CatFeatures size",
            /*internalCheck*/ true
        );
    }
    CheckDataSizes(objectCount, featuresLayout, EFeatureType::Categorical, CatFeatures);

    localExecutor->ExecRangeWithThrow(
        [&] (int catFeatureIdx) {
            auto* catFeaturePtr = CatFeatures[catFeatureIdx].Get();
            if (catFeaturePtr) {
                const auto& hashToStringMap = (*CatFeaturesHashToString)[catFeatureIdx];
                catFeaturePtr->GetArrayData().ParallelForEach(
                    [&] (ui32 objectIdx, ui32 hashValue) {
                        CB_ENSURE_INTERNAL(
                            hashToStringMap.has(hashValue),
                            "catFeature #" << catFeatureIdx << ", object #" << objectIdx << ": value "
                            << Hex(hashValue) << " is missing from CatFeaturesHashToString"
                        );
                    },
                    localExecutor
                );
            }
        },
        0,
        SafeIntegerCast<int>(CatFeatures.size()),
        NPar::TLocalExecutor::WAIT_COMPLETE
    );
}


template <class T, EFeatureValuesType TType>
static void CreateSubsetFeatures(
    TConstArrayRef<THolder<TArrayValuesHolder<T, TType>>> src,
    const TFeaturesArraySubsetIndexing* subsetIndexing,
    TVector<THolder<TArrayValuesHolder<T, TType>>>* dst
) {
    dst->clear();
    dst->reserve(src.size());
    for (const auto& feature : src) {
        auto* srcDataPtr = feature.Get();
        if (srcDataPtr) {
            dst->emplace_back(
                MakeHolder<TArrayValuesHolder<T, TType>>(
                    srcDataPtr->GetId(),
                    *(srcDataPtr->GetArrayData().GetSrc()),
                    subsetIndexing
                )
            );
        } else {
            dst->push_back(nullptr);
        }
    }
}


TObjectsDataProviderPtr NCB::TRawObjectsDataProvider::GetSubset(
    const TObjectsGroupingSubset& objectsGroupingSubset,
    NPar::TLocalExecutor* localExecutor
) const {
    TCommonObjectsData subsetCommonData = CommonData.GetSubset(
        objectsGroupingSubset,
        localExecutor
    );

    TRawObjectsData subsetData;
    CreateSubsetFeatures(
        (TConstArrayRef<THolder<TFloatValuesHolder>>)Data.FloatFeatures,
        subsetCommonData.SubsetIndexing.Get(),
        &subsetData.FloatFeatures
    );
    CreateSubsetFeatures(
        (TConstArrayRef<THolder<THashedCatValuesHolder>>)Data.CatFeatures,
        subsetCommonData.SubsetIndexing.Get(),
        &subsetData.CatFeatures
    );

    subsetData.CatFeaturesHashToString = Data.CatFeaturesHashToString;

    return MakeIntrusive<TRawObjectsDataProvider>(
        objectsGroupingSubset.GetSubsetGrouping(),
        std::move(subsetCommonData),
        std::move(subsetData),
        true,
        Nothing()
    );
}


void NCB::TRawObjectsDataProvider::SetGroupIds(TConstArrayRef<TStringBuf> groupStringIds) {
    CheckDataSize(groupStringIds.size(), (size_t)GetObjectCount(), "group Ids");

    TVector<TGroupId> newGroupIds;
    newGroupIds.yresize(groupStringIds.size());
    for (auto i : xrange(groupStringIds.size())) {
        newGroupIds[i] = CalcGroupIdFor(groupStringIds[i]);
    }

    ObjectsGrouping = MakeIntrusive<TObjectsGrouping>(
        CreateObjectsGroupingFromGroupIds(GetObjectCount(), (TConstArrayRef<TGroupId>)newGroupIds)
    );
    CommonData.GroupIds = std::move(newGroupIds);
}

void NCB::TRawObjectsDataProvider::SetSubgroupIds(TConstArrayRef<TStringBuf> subgroupStringIds) {
    CheckDataSize(subgroupStringIds.size(), (size_t)GetObjectCount(), "subgroup Ids");
    CB_ENSURE(
        CommonData.GroupIds,
        "non-empty subgroupStringIds when GroupIds is not defined"
    );

    TVector<TSubgroupId> newSubgroupIds;
    newSubgroupIds.yresize(subgroupStringIds.size());
    for (auto i : xrange(subgroupStringIds.size())) {
        newSubgroupIds[i] = CalcSubgroupIdFor(subgroupStringIds[i]);
    }
    CommonData.SubgroupIds = std::move(newSubgroupIds);
}



void NCB::TQuantizedObjectsData::PrepareForInitialization(
    const TDataMetaInfo& metaInfo,
    const NCatboostOptions::TBinarizationOptions& binarizationOptions
) {
    // FloatFeatures and CatFeatures members are initialized at the end of building
    FloatFeatures.clear();
    FloatFeatures.resize(metaInfo.FeaturesLayout->GetFloatFeatureCount());

    CatFeatures.clear();
    const ui32 catFeatureCount = metaInfo.FeaturesLayout->GetCatFeatureCount();
    CatFeatures.resize(catFeatureCount);

    if (!QuantizedFeaturesInfo) {
        QuantizedFeaturesInfo = MakeIntrusive<TQuantizedFeaturesInfo>(
            metaInfo.FeaturesLayout,
            binarizationOptions
        );
    }
}


void NCB::TQuantizedObjectsData::Check(
    ui32 objectCount,
    const TFeaturesLayout& featuresLayout,
    NPar::TLocalExecutor* localExecutor
) const {
    /* localExecutor is a parameter here to make
     * TQuantizedObjectsData::Check and TQuantizedObjectsData::Check have the same interface
     */
    Y_UNUSED(localExecutor);

    CB_ENSURE(QuantizedFeaturesInfo.Get(), "NCB::TQuantizedObjectsData::QuantizedFeaturesInfo is not initialized");

    CheckDataSizes(objectCount, featuresLayout, EFeatureType::Float, FloatFeatures);
    CheckDataSizes(objectCount, featuresLayout, EFeatureType::Categorical, CatFeatures);
}


template <class T>
static void CreateSubsetFeatures(
    const TVector<THolder<T>>& src, // not TConstArrayRef to allow template parameter deduction
    const TFeaturesArraySubsetIndexing* subsetIndexing,
    TVector<THolder<T>>* dst
) {
    dst->clear();
    dst->reserve(src.size());
    for (const auto& feature : src) {
        auto* srcDataPtr = feature.Get();
        if (srcDataPtr) {
            dst->emplace_back(srcDataPtr->CloneWithNewSubsetIndexing(subsetIndexing));
        } else {
            dst->push_back(nullptr);
        }
    }
}

TQuantizedObjectsData NCB::TQuantizedObjectsData::GetSubset(
    const TArraySubsetIndexing<ui32>* subsetComposition
) const {
    TQuantizedObjectsData subsetData;
    CreateSubsetFeatures(
        FloatFeatures,
        subsetComposition,
        &subsetData.FloatFeatures
    );
    CreateSubsetFeatures(
        CatFeatures,
        subsetComposition,
        &subsetData.CatFeatures
    );
    subsetData.QuantizedFeaturesInfo = QuantizedFeaturesInfo;

    return subsetData;
}

template <EFeatureType FeatureType, class T, class IColumn>
static ui32 CalcFeatureValuesCheckSum(
    ui32 init,
    const TFeaturesLayout& featuresLayout,
    const TVector<THolder<IColumn>>& featuresData,
    NPar::TLocalExecutor* localExecutor)
{
    ui32 checkSum = init;
    const ui32 emptyColumnDataForCrc = 0;
    for (auto perTypeFeatureIdx : xrange(featuresLayout.GetFeatureCount(FeatureType))) {
        if (featuresLayout.GetInternalFeatureMetaInfo(perTypeFeatureIdx, FeatureType).IsAvailable) {
            if (auto compressedValuesFeatureData = dynamic_cast<const TCompressedValuesHolderImpl<IColumn>*>(
                    featuresData[perTypeFeatureIdx].Get()
                ))
            {
                compressedValuesFeatureData->GetArrayData().ForEach([&](ui32 /*idx*/, T element) {
                    checkSum = UpdateCheckSum(checkSum, element);
                });
            } else {
                const auto valuesFeatureData = featuresData[perTypeFeatureIdx]->ExtractValues(localExecutor);
                for (auto element : *valuesFeatureData) {
                    checkSum = UpdateCheckSum(checkSum, element);
                }
            }
        } else {
            checkSum = UpdateCheckSum(checkSum, emptyColumnDataForCrc);
        }
    }
    return checkSum;
}

ui32 NCB::TQuantizedObjectsDataProvider::CalcFeaturesCheckSum(NPar::TLocalExecutor* localExecutor) const {
    ui32 checkSum = 0;

    checkSum = Data.QuantizedFeaturesInfo->CalcCheckSum();
    checkSum = CalcFeatureValuesCheckSum<EFeatureType::Float, ui8>(
        checkSum,
        *CommonData.FeaturesLayout,
        Data.FloatFeatures,
        localExecutor
    );
    checkSum = CalcFeatureValuesCheckSum<EFeatureType::Categorical, ui32>(
        checkSum,
        *CommonData.FeaturesLayout,
        Data.CatFeatures,
        localExecutor
    );

    return checkSum;

}

template <EFeatureType FeatureType, class IColumnType>
static void LoadFeatures(
    const TFeaturesLayout& featuresLayout,
    const TFeaturesArraySubsetIndexing* subsetIndexing,
    IBinSaver* binSaver,
    TVector<THolder<IColumnType>>* dst
) {
    const ui32 objectCount = subsetIndexing->Size();

    dst->clear();
    dst->resize(featuresLayout.GetFeatureCount(FeatureType));

    featuresLayout.IterateOverAvailableFeatures<FeatureType>(
        [&] (TFeatureIdx<FeatureType> featureIdx) {
            ui32 flatFeatureIdx = featuresLayout.GetExternalFeatureIdx(*featureIdx, FeatureType);

            ui32 id;
            ui32 size;
            ui32 bitsPerKey;
            LoadMulti(binSaver, &id, &size, &bitsPerKey);

            CB_ENSURE_INTERNAL(
                flatFeatureIdx == id,
                "deserialized feature id is not equal to expected flatFeatureIdx"
            );
            CheckDataSize(size, objectCount, "column data", false, "object count", true);

            TVector<ui64> storage;
            LoadMulti(binSaver, &storage);

            (*dst)[*featureIdx] = MakeHolder<TCompressedValuesHolderImpl<IColumnType>>(
                flatFeatureIdx,
                TCompressedArray(
                    objectCount,
                    bitsPerKey,
                    TMaybeOwningArrayHolder<ui64>::CreateOwning(std::move(storage))
                ),
                subsetIndexing
            );
        }
    );
}

void NCB::TQuantizedObjectsData::Load(
    const TArraySubsetIndexing<ui32>* subsetIndexing,
    NCB::TQuantizedFeaturesInfoPtr quantizedFeaturesInfo,
    IBinSaver* binSaver
) {
    QuantizedFeaturesInfo = quantizedFeaturesInfo;
    LoadFeatures<EFeatureType::Float>(
        QuantizedFeaturesInfo->GetFeaturesLayout(),
        subsetIndexing,
        binSaver,
        &FloatFeatures
    );
    LoadFeatures<EFeatureType::Categorical>(
        QuantizedFeaturesInfo->GetFeaturesLayout(),
        subsetIndexing,
        binSaver,
        &CatFeatures
    );
}


template <EFeatureType FeatureType, class IColumnType>
static void SaveFeatures(
    const TFeaturesLayout& featuresLayout,
    const TVector<THolder<IColumnType>>& src,
    NPar::TLocalExecutor* localExecutor,
    IBinSaver* binSaver
) {
    constexpr ui8 paddingBuffer[sizeof(ui64)-1] = {0};

    featuresLayout.IterateOverAvailableFeatures<FeatureType>(
        [&] (TFeatureIdx<FeatureType> featureIdx) {
            // TODO(akhropov): replace by repacking (possibly in parts) to compressed array in the future
            const auto values = src[*featureIdx]->ExtractValues(localExecutor);
            const ui32 objectCount = (*values).size();
            const ui32 bytesPerKey = sizeof(*(*values).data());
            const ui32 bitsPerKey = bytesPerKey*8;
            SaveMulti(binSaver, src[*featureIdx]->GetId(), objectCount, bitsPerKey);

            TIndexHelper<ui64> indexHelper(bitsPerKey);

            // save values to be deserialiable as a TVector<ui64>

            const IBinSaver::TStoredSize compressedStorageVectorSize = indexHelper.CompressedSize(objectCount);
            SaveMulti(binSaver, compressedStorageVectorSize);

            // pad to ui64-alignment to make it deserializable as CompressedArray storage
            const size_t paddingSize =
                size_t(compressedStorageVectorSize)*sizeof(ui64) - size_t(bytesPerKey)*objectCount;

            SaveRawData(*values, binSaver);
            if (paddingSize) {
                SaveRawData(TConstArrayRef<ui8>(paddingBuffer, paddingSize), binSaver);
            }
        }
    );
}

void NCB::TQuantizedObjectsData::SaveNonSharedPart(IBinSaver* binSaver) const {
    NPar::TLocalExecutor localExecutor;

    SaveFeatures<EFeatureType::Float>(
        QuantizedFeaturesInfo->GetFeaturesLayout(),
        FloatFeatures,
        &localExecutor,
        binSaver
    );
    SaveFeatures<EFeatureType::Categorical>(
        QuantizedFeaturesInfo->GetFeaturesLayout(),
        CatFeatures,
        &localExecutor,
        binSaver
    );
}


NCB::TQuantizedForCPUObjectsDataProvider::TQuantizedForCPUObjectsDataProvider(
    TMaybe<TObjectsGroupingPtr> objectsGrouping,
    TCommonObjectsData&& commonData,
    TQuantizedObjectsData&& data,
    bool skipCheck,
    TMaybe<NPar::TLocalExecutor*> localExecutor
)
    : TQuantizedObjectsDataProvider(
        std::move(objectsGrouping),
        std::move(commonData),
        std::move(data),
        skipCheck,
        localExecutor
      )
{
    if (!skipCheck) {
        Check();
    }

    CatFeatureUniqueValuesCounts.yresize(Data.CatFeatures.size());
    for (auto catFeatureIdx : xrange(Data.CatFeatures.size())) {
        CatFeatureUniqueValuesCounts[catFeatureIdx] =
            Data.QuantizedFeaturesInfo->GetUniqueValuesCounts(TCatFeatureIdx(catFeatureIdx));
    }
}


template <class TRequiredFeatureColumn, class TRawArrayType, class TBaseFeatureColumn>
static void CheckIsRequiredType(
    EFeatureType featureType,
    // not TConstArrayRef to allow template parameter deduction
    const TVector<THolder<TBaseFeatureColumn>>& data,
    const TStringBuf requiredTypeName
) {
    for (auto featureIdx : xrange(data.size())) {
        auto* dataPtr = data[featureIdx].Get();
        if (!dataPtr) {
            continue;
        }

        auto requiredTypePtr = dynamic_cast<TRequiredFeatureColumn*>(dataPtr);
        CB_ENSURE_INTERNAL(
            requiredTypePtr,
            "Data." << featureType << "Features[" << featureIdx << "] is not of type " << requiredTypeName
        );
        requiredTypePtr->GetCompressedData().GetSrc()
            ->template CheckIfCanBeInterpretedAsRawArray<TRawArrayType>();
    }
}


void NCB::TQuantizedForCPUObjectsDataProvider::Check() const {
    try {
        CheckIsRequiredType<TQuantizedFloatValuesHolder, ui8>(
            EFeatureType::Float,
            Data.FloatFeatures,
            "TQuantizedFloatValuesHolder"
        );
        CheckIsRequiredType<TQuantizedCatValuesHolder, ui32>(
            EFeatureType::Categorical,
            Data.CatFeatures,
            "TQuantizedCatValuesHolder"
        );
    } catch (const TCatboostException& e) {
        // not ythrow to avoid double line info in exception message
        throw TCatboostException() << "Incompatible with TQuantizedForCPUObjectsDataProvider: " << e.what();
    }
}
