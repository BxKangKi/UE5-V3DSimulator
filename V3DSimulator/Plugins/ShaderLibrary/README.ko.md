# ShaderLibrary

셀 셰이딩 후처리와 Toon 스텐실 서브시스템은 제거했습니다. MI_Toon은 프로젝트가 완성한 표면 머터리얼이며 이 플러그인의 후처리에 의존하지 않습니다. /Plugin/ShaderLibrary가 셰이더 경로입니다.

수중 후처리는 Content/Water/PostProcessing/MPP_UnderWater를 사용합니다. MID의 WaterLevel에 수면 높이(cm)를 전달하고, 물이 없으면 후처리 블렌드 가중치를 0으로 설정하세요. Underwater.ush의 광선·흐림은 SceneDepth(1)를 사용합니다. Opaque/Masked 메시가 차폐하며, 화면 밖의 가림이나 SceneDepth를 쓰지 않는 Translucent 표면은 이 화면 공간 방식에서 추적하지 않습니다. 굴절 전후 깊이를 보수적으로 검사해 플레이어 실루엣을 통과하는 광선을 줄입니다.

PostProcess.ush는 PP_Weather용 RS_WeatherPostProcess만 제공합니다. ShaderLibrary.ush는 공용 노멀/투영 함수와 후처리에서 사용할 수중 함수를 제공합니다. Editor 시작 시 MPC_ShaderLibrary의 현재 파라미터를 준비하고 사용하지 않는 셀 셰이딩 항목을 제거·저장합니다.

완성된 표면 머터리얼과 MIC는 생성·변경하지 않습니다. Unreal/셰이더 컴파일 및 시각 검증은 이 환경에서 수행하지 못했습니다. 적용 방법과 확인 항목은 전체 프로젝트 Docs/STABILIZATION_2026-11-19.ko.md를 참조하세요.

플러그인만 갱신할 때에도 기존 ShaderLibrary 폴더를 교체하세요. 파일 덮어쓰기만 하면 삭제된 Toon 소스가 남습니다.
