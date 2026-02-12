// Copyright Druid Mechanics

/**
 * @file ExecCalc_Damage.cpp
 * @brief 伤害计算执行类实现文件
 * 
 * 这个类负责处理游戏中的伤害计算逻辑，包括：
 * - 基础伤害计算
 * - 护甲和护甲穿透
 * - 暴击和暴击抗性
 * - 格挡机制
 * - 元素抗性
 * - 减益效果判定
 * - 范围伤害处理
 */

#include "AbilitySystem/ExecCalc/ExecCalc_Damage.h"

#include "AbilitySystemComponent.h"
#include "AuraAbilityTypes.h"
#include "AuraGameplayTags.h"
#include "AbilitySystem/AuraAbilitySystemLibrary.h"
#include "AbilitySystem/AuraAttributeSet.h"
#include "AbilitySystem/Data/CharacterClassInfo.h"
#include "Camera/CameraShakeSourceActor.h"
#include "Interaction/CombatInterface.h"
#include "Kismet/GameplayStatics.h"

/**
 * @struct AuraDamageStatics
 * @brief 静态伤害属性捕获定义结构体
 * 
 * 这个结构体定义了所有需要在伤害计算中捕获的属性，
 * 包括防御属性、攻击属性和元素抗性属性
 */
struct AuraDamageStatics
{
	// 防御属性声明
	DECLARE_ATTRIBUTE_CAPTUREDEF(Armor);           // 护甲值
	DECLARE_ATTRIBUTE_CAPTUREDEF(ArmorPenetration); // 护甲穿透
	DECLARE_ATTRIBUTE_CAPTUREDEF(BlockChance);     // 格挡几率
	
	// 暴击相关属性声明
	DECLARE_ATTRIBUTE_CAPTUREDEF(CriticalHitChance);      // 暴击几率
	DECLARE_ATTRIBUTE_CAPTUREDEF(CriticalHitResistance);  // 暴击抗性
	DECLARE_ATTRIBUTE_CAPTUREDEF(CriticalHitDamage);      // 暴击伤害加成
	
	// 元素抗性属性声明
	DECLARE_ATTRIBUTE_CAPTUREDEF(FireResistance);      // 火焰抗性
	DECLARE_ATTRIBUTE_CAPTUREDEF(LightningResistance); // 闪电抗性
	DECLARE_ATTRIBUTE_CAPTUREDEF(ArcaneResistance);    // 奥术抗性
	DECLARE_ATTRIBUTE_CAPTUREDEF(PhysicalResistance);  // 物理抗性
	
	/**
	 * @brief 构造函数，定义所有属性的捕获规则
	 */
	AuraDamageStatics()
	{
		// 防御属性定义（目标属性）
		DEFINE_ATTRIBUTE_CAPTUREDEF(UAuraAttributeSet, Armor, Target, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UAuraAttributeSet, BlockChance, Target, false);
		
		// 攻击属性定义（来源属性）
		DEFINE_ATTRIBUTE_CAPTUREDEF(UAuraAttributeSet, ArmorPenetration, Source, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UAuraAttributeSet, CriticalHitChance, Source, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UAuraAttributeSet, CriticalHitResistance, Target, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UAuraAttributeSet, CriticalHitDamage, Source, false);

		// 元素抗性定义（目标属性）
		DEFINE_ATTRIBUTE_CAPTUREDEF(UAuraAttributeSet, FireResistance, Target, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UAuraAttributeSet, LightningResistance, Target, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UAuraAttributeSet, ArcaneResistance, Target, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UAuraAttributeSet, PhysicalResistance, Target, false);
	}
};

/**
 * @brief 获取伤害统计静态实例
 * @return 返回AuraDamageStatics的单例引用
 * 
 * 使用静态局部变量确保线程安全的单例模式
 */
static const AuraDamageStatics& DamageStatics()
{
	static AuraDamageStatics DStatics;
	return DStatics;
}

/**
 * @brief 构造函数，注册需要捕获的相关属性
 * 
 * 在Gameplay Effect执行计算时，这些属性会被自动捕获并可用于计算
 */
UExecCalc_Damage::UExecCalc_Damage()
{
	// 注册防御属性
	RelevantAttributesToCapture.Add(DamageStatics().ArmorDef);
	RelevantAttributesToCapture.Add(DamageStatics().BlockChanceDef);
	RelevantAttributesToCapture.Add(DamageStatics().ArmorPenetrationDef);
	
	// 注册暴击相关属性
	RelevantAttributesToCapture.Add(DamageStatics().CriticalHitChanceDef);
	RelevantAttributesToCapture.Add(DamageStatics().CriticalHitResistanceDef);
	RelevantAttributesToCapture.Add(DamageStatics().CriticalHitDamageDef);

	// 注册元素抗性属性
	RelevantAttributesToCapture.Add(DamageStatics().FireResistanceDef);
	RelevantAttributesToCapture.Add(DamageStatics().LightningResistanceDef);
	RelevantAttributesToCapture.Add(DamageStatics().ArcaneResistanceDef);
	RelevantAttributesToCapture.Add(DamageStatics().PhysicalResistanceDef);
}

/**
 * @brief 确定是否应用减益效果
 * @param ExecutionParams 执行参数
 * @param Spec Gameplay Effect规格
 * @param EvaluationParameters 评估参数
 * @param InTagsToDefs 标签到属性定义的映射
 * 
 * 这个方法检查每种伤害类型是否应该应用对应的减益效果，
 * 考虑来源的减益几率和目标的减益抗性
 */
void UExecCalc_Damage::DetermineDebuff(const FGameplayEffectCustomExecutionParameters& ExecutionParams, const FGameplayEffectSpec& Spec, FAggregatorEvaluateParameters EvaluationParameters,
						 const TMap<FGameplayTag, FGameplayEffectAttributeCaptureDefinition>& InTagsToDefs) const
{
	const FAuraGameplayTags& GameplayTags = FAuraGameplayTags::Get();

	// 遍历所有伤害类型到减益效果的映射
	for (TTuple<FGameplayTag, FGameplayTag> Pair : GameplayTags.DamageTypesToDebuffs)
	{
		const FGameplayTag& DamageType = Pair.Key;    // 伤害类型标签
		const FGameplayTag& DebuffType = Pair.Value;  // 对应的减益类型标签
		
		// 获取该伤害类型的伤害值
		const float TypeDamage = Spec.GetSetByCallerMagnitude(DamageType, false, -1.f);
		if (TypeDamage > -.5f) // .5 容差用于浮点数精度处理
		{
			// 获取来源的减益几率
			const float SourceDebuffChance = Spec.GetSetByCallerMagnitude(GameplayTags.Debuff_Chance, false, -1.f);

			// 获取目标的减益抗性
			float TargetDebuffResistance = 0.f;
			const FGameplayTag& ResistanceTag = GameplayTags.DamageTypesToResistances[DamageType];
			ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(InTagsToDefs[ResistanceTag], EvaluationParameters, TargetDebuffResistance);
			TargetDebuffResistance = FMath::Max<float>(TargetDebuffResistance, 0.f);
			
			// 计算有效减益几率：来源几率 * (100 - 目标抗性) / 100
			const float EffectiveDebuffChance = SourceDebuffChance * ( 100 - TargetDebuffResistance ) / 100.f;
			
			// 随机判定是否应用减益
			const bool bDebuff = FMath::RandRange(1, 100) < EffectiveDebuffChance;
			
			if (bDebuff)
			{
				FGameplayEffectContextHandle ContextHandle = Spec.GetContext();

				// 设置减益相关参数到上下文
				UAuraAbilitySystemLibrary::SetIsSuccessfulDebuff(ContextHandle, true);
				UAuraAbilitySystemLibrary::SetDamageType(ContextHandle, DamageType);

				// 获取减益参数
				const float DebuffDamage = Spec.GetSetByCallerMagnitude(GameplayTags.Debuff_Damage, false, -1.f);
				const float DebuffDuration = Spec.GetSetByCallerMagnitude(GameplayTags.Debuff_Duration, false, -1.f);
				const float DebuffFrequency = Spec.GetSetByCallerMagnitude(GameplayTags.Debuff_Frequency, false, -1.f);

				// 设置减益参数到上下文
				UAuraAbilitySystemLibrary::SetDebuffDamage(ContextHandle, DebuffDamage);
				UAuraAbilitySystemLibrary::SetDebuffDuration(ContextHandle, DebuffDuration);
				UAuraAbilitySystemLibrary::SetDebuffFrequency(ContextHandle, DebuffFrequency);
			}
		}
	}
}

/**
 * @brief 主要的伤害计算执行函数
 * @param ExecutionParams 执行参数
 * @param OutExecutionOutput 输出执行结果
 * 
 * 这是Gameplay Ability System的核心伤害计算逻辑，按以下顺序处理：
 * 1. 获取来源和目标信息
 * 2. 判定减益效果
 * 3. 计算基础伤害（考虑元素抗性）
 * 4. 处理范围伤害
 * 5. 判定格挡
 * 6. 计算护甲减伤
 * 7. 判定暴击
 * 8. 输出最终伤害值
 */
void UExecCalc_Damage::Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams,
                                              FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const
{
	// 创建标签到属性定义的映射表
	TMap<FGameplayTag, FGameplayEffectAttributeCaptureDefinition> TagsToCaptureDefs;
	const FAuraGameplayTags& Tags = FAuraGameplayTags::Get();
		
	// 映射防御属性标签
	TagsToCaptureDefs.Add(Tags.Attributes_Secondary_Armor, DamageStatics().ArmorDef);
	TagsToCaptureDefs.Add(Tags.Attributes_Secondary_BlockChance, DamageStatics().BlockChanceDef);
	TagsToCaptureDefs.Add(Tags.Attributes_Secondary_ArmorPenetration, DamageStatics().ArmorPenetrationDef);
	
	// 映射暴击相关属性标签
	TagsToCaptureDefs.Add(Tags.Attributes_Secondary_CriticalHitChance, DamageStatics().CriticalHitChanceDef);
	TagsToCaptureDefs.Add(Tags.Attributes_Secondary_CriticalHitResistance, DamageStatics().CriticalHitResistanceDef);
	TagsToCaptureDefs.Add(Tags.Attributes_Secondary_CriticalHitDamage, DamageStatics().CriticalHitDamageDef);

	// 映射元素抗性属性标签
	TagsToCaptureDefs.Add(Tags.Attributes_Resistance_Arcane, DamageStatics().ArcaneResistanceDef);
	TagsToCaptureDefs.Add(Tags.Attributes_Resistance_Fire, DamageStatics().FireResistanceDef);
	TagsToCaptureDefs.Add(Tags.Attributes_Resistance_Lightning, DamageStatics().LightningResistanceDef);
	TagsToCaptureDefs.Add(Tags.Attributes_Resistance_Physical, DamageStatics().PhysicalResistanceDef);
	
	
	// 获取来源和目标的能力系统组件
	const UAbilitySystemComponent* SourceASC = ExecutionParams.GetSourceAbilitySystemComponent();
	const UAbilitySystemComponent* TargetASC = ExecutionParams.GetTargetAbilitySystemComponent();

	// 获取来源和目标的Avatar角色
	AActor* SourceAvatar = SourceASC ? SourceASC->GetAvatarActor() : nullptr;
	AActor* TargetAvatar = TargetASC ? TargetASC->GetAvatarActor() : nullptr;

	// 获取来源玩家等级
	int32 SourcePlayerLevel = 1;
	if (SourceAvatar->Implements<UCombatInterface>())
	{
		SourcePlayerLevel = ICombatInterface::Execute_GetPlayerLevel(SourceAvatar);
	}
	
	// 获取目标玩家等级
	int32 TargetPlayerLevel = 1;
	if (TargetAvatar->Implements<UCombatInterface>())
	{
		TargetPlayerLevel = ICombatInterface::Execute_GetPlayerLevel(TargetAvatar);
	}

	// 获取Gameplay Effect规格和上下文
	const FGameplayEffectSpec& Spec = ExecutionParams.GetOwningSpec();
	FGameplayEffectContextHandle EffectContextHandle = Spec.GetContext();

	// 设置评估参数（标签信息）
	const FGameplayTagContainer* SourceTags = Spec.CapturedSourceTags.GetAggregatedTags();
	const FGameplayTagContainer* TargetTags = Spec.CapturedTargetTags.GetAggregatedTags();
	FAggregatorEvaluateParameters EvaluationParameters;
	EvaluationParameters.SourceTags = SourceTags;
	EvaluationParameters.TargetTags = TargetTags;

	// 步骤1: 判定减益效果
	DetermineDebuff(ExecutionParams, Spec, EvaluationParameters, TagsToCaptureDefs);

	// 步骤2: 计算基础伤害（考虑元素抗性）
	float Damage = 0.f;
	for (const TTuple<FGameplayTag, FGameplayTag>& Pair  : FAuraGameplayTags::Get().DamageTypesToResistances)
	{
		const FGameplayTag DamageTypeTag = Pair.Key;    // 伤害类型标签
		const FGameplayTag ResistanceTag = Pair.Value;  // 对应的抗性标签
		
		// 安全检查：确保映射表中包含该抗性标签
		checkf(TagsToCaptureDefs.Contains(ResistanceTag), TEXT("TagsToCaptureDefs doesn't contain Tag: [%s] in ExecCalc_Damage"), *ResistanceTag.ToString());
		const FGameplayEffectAttributeCaptureDefinition CaptureDef = TagsToCaptureDefs[ResistanceTag];

		// 获取该伤害类型的伤害值
		float DamageTypeValue = Spec.GetSetByCallerMagnitude(Pair.Key, false);
		if (DamageTypeValue <= 0.f)
		{
			continue; // 跳过无效伤害值
		}
		
		// 获取目标的对应抗性值
		float Resistance = 0.f;
		ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(CaptureDef, EvaluationParameters, Resistance);
		Resistance = FMath::Clamp(Resistance, 0.f, 100.f); // 限制抗性在0-100范围内

		// 根据抗性调整伤害值：伤害值 * (100 - 抗性) / 100
		DamageTypeValue *= ( 100.f - Resistance ) / 100.f;

		// 步骤3: 处理范围伤害
		if (UAuraAbilitySystemLibrary::IsRadialDamage(EffectContextHandle))
		{
			// 范围伤害处理逻辑：
			// 1. 在AuraCharacterBase中重写TakeDamage方法
			// 2. 创建OnDamageDelegate委托，在TakeDamage中广播伤害接收事件
			// 3. 在这里为目标绑定OnDamageDelegate的lambda函数
			// 4. 调用ApplyRadialDamageWithFalloff应用范围伤害
			// 5. 在lambda函数中，从广播事件获取实际伤害值
			
			if (ICombatInterface* CombatInterface = Cast<ICombatInterface>(TargetAvatar))
			{
				// 绑定伤害接收委托
				CombatInterface->GetOnDamageSignature().AddLambda([&](float DamageAmount)
				{
					DamageTypeValue = DamageAmount; // 更新为实际接收的伤害值
				});
			}
			
			// 应用范围伤害
			UGameplayStatics::ApplyRadialDamageWithFalloff(
				TargetAvatar,                                    // 目标角色
				DamageTypeValue,                                 // 基础伤害值
				0.f,                                             // 最小伤害值
				UAuraAbilitySystemLibrary::GetRadialDamageOrigin(EffectContextHandle),     // 伤害源点
				UAuraAbilitySystemLibrary::GetRadialDamageInnerRadius(EffectContextHandle), // 内半径
				UAuraAbilitySystemLibrary::GetRadialDamageOuterRadius(EffectContextHandle), // 外半径
				1.f,                                             // 伤害衰减系数
				UDamageType::StaticClass(),                      // 伤害类型
				TArray<AActor*>(),                               // 忽略的角色列表
				SourceAvatar,                                    // 伤害来源
				nullptr);                                        // 伤害实例
		}
		
		// 累加该伤害类型的伤害值到总伤害
		Damage += DamageTypeValue;
	}

	// 步骤4: 判定格挡
	float TargetBlockChance = 0.f;
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().BlockChanceDef, EvaluationParameters, TargetBlockChance);
	TargetBlockChance = FMath::Max<float>(TargetBlockChance, 0.f);

	// 随机判定是否成功格挡
	const bool bBlocked = FMath::RandRange(1, 100) < TargetBlockChance;
	
	// 设置格挡状态到上下文
	UAuraAbilitySystemLibrary::SetIsBlockedHit(EffectContextHandle, bBlocked);

	// 如果格挡成功，伤害减半
	Damage = bBlocked ? Damage / 2.f : Damage;
	
	// 步骤5: 计算护甲减伤
	float TargetArmor = 0.f;
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().ArmorDef, EvaluationParameters, TargetArmor);
	TargetArmor = FMath::Max<float>(TargetArmor, 0.f);

	float SourceArmorPenetration = 0.f;
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().ArmorPenetrationDef, EvaluationParameters, SourceArmorPenetration);
	SourceArmorPenetration = FMath::Max<float>(SourceArmorPenetration, 0.f);

	// 获取角色类别信息中的伤害计算系数
	const UCharacterClassInfo* CharacterClassInfo = UAuraAbilitySystemLibrary::GetCharacterClassInfo(SourceAvatar);
	
	// 获取护甲穿透系数曲线（基于来源等级）
	const FRealCurve* ArmorPenetrationCurve = CharacterClassInfo->DamageCalculationCoefficients->FindCurve(FName("ArmorPenetration"), FString());
	const float ArmorPenetrationCoefficient = ArmorPenetrationCurve->Eval(SourcePlayerLevel);
	
	// 计算有效护甲：目标护甲 * (100 - 来源护甲穿透 * 系数) / 100
	const float EffectiveArmor = TargetArmor * ( 100 - SourceArmorPenetration * ArmorPenetrationCoefficient ) / 100.f;

	// 获取有效护甲系数曲线（基于目标等级）
	const FRealCurve* EffectiveArmorCurve = CharacterClassInfo->DamageCalculationCoefficients->FindCurve(FName("EffectiveArmor"), FString());
	const float EffectiveArmorCoefficient = EffectiveArmorCurve->Eval(TargetPlayerLevel);
	
	// 根据有效护甲减少伤害：伤害 * (100 - 有效护甲 * 系数) / 100
	Damage *= ( 100 - EffectiveArmor * EffectiveArmorCoefficient ) / 100.f;

	// 步骤6: 判定暴击
	float SourceCriticalHitChance = 0.f;
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().CriticalHitChanceDef, EvaluationParameters, SourceCriticalHitChance);
	SourceCriticalHitChance = FMath::Max<float>(SourceCriticalHitChance, 0.f);
	
	float TargetCriticalHitResistance = 0.f;
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().CriticalHitResistanceDef, EvaluationParameters, TargetCriticalHitResistance);
	TargetCriticalHitResistance = FMath::Max<float>(TargetCriticalHitResistance, 0.f);

	float SourceCriticalHitDamage = 0.f;
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().CriticalHitDamageDef, EvaluationParameters, SourceCriticalHitDamage);
	SourceCriticalHitDamage = FMath::Max<float>(SourceCriticalHitDamage, 0.f);

	// 获取暴击抗性系数曲线（基于目标等级）
	const FRealCurve* CriticalHitResistanceCurve = CharacterClassInfo->DamageCalculationCoefficients->FindCurve(FName("CriticalHitResistance"), FString());
	const float CriticalHitResistanceCoefficient = CriticalHitResistanceCurve->Eval(TargetPlayerLevel);

	// 计算有效暴击几率：来源暴击几率 - 目标暴击抗性 * 系数
	const float EffectiveCriticalHitChance = SourceCriticalHitChance - TargetCriticalHitResistance * CriticalHitResistanceCoefficient;
	
	// 随机判定是否暴击
	const bool bCriticalHit = FMath::RandRange(1, 100) < EffectiveCriticalHitChance;

	// 设置暴击状态到上下文
	UAuraAbilitySystemLibrary::SetIsCriticalHit(EffectContextHandle, bCriticalHit);

	// 如果暴击：伤害 = 2倍伤害 + 暴击伤害加成
	Damage = bCriticalHit ? 2.f * Damage + SourceCriticalHitDamage : Damage;
	
	// 步骤7: 输出最终伤害值到Gameplay Effect系统
	const FGameplayModifierEvaluatedData EvaluatedData(UAuraAttributeSet::GetIncomingDamageAttribute(), EGameplayModOp::Additive, Damage);
	OutExecutionOutput.AddOutputModifier(EvaluatedData);
}