# Aoi YUME 음질·UI/UX 업그레이드 계획

기준: 2026-09-29, 현재 워크트리(hardcoded 수정 3건 + 기존 미커밋 변경 포함).
`aoi-yume-quality-review-plan-2026-09-22.md`가 구조적 품질 게이트(P1~P5)를 다룬다면,
이 문서는 그 위에 올라가는 **음악적 표현력과 사용 경험** 개선안이다.

## 현재 체인과 진단

체인: voices(48-tap sinc 보간, TPT SVF, 선형 ADSR, curve drive) -> 스테레오 레인
-> CC gain/pan -> bus(tape/fold/공진 LPF @OS 1-8x) -> 컴프레서 -> FX(chorus/Freeverb/
핑퐁 딜레이) -> trim -> safety ceiling.

### 음질 격차 (영향 큰 순서)

1. **SF2 표현력의 절반 이상을 엔진이 무시한다.** Flattener는 이미
   `volumeEnvelope`, `attenuationDb`, `pan`, `exclusiveClass`, `modEnvToFilter`,
   `modEnvToPitch`, modulation envelope, velocity override까지 계산해 두는데,
   `SF2Loader`가 `Sample`에 복사하는 것은 pitch/filter/loop뿐이다. GM 뱅크의
   악기 캐릭터(하이햇 초크, 음색 레이어별 음량/팬, 음색별 릴리즈)가 사라진다.
   - 레이어링: `getSample()`이 매치 배열 크기 1이라 velocity/key에 겹치는 존은
     첫 매치만 울린다. 스네어(스틱+림), 그랜드(p/ff 레이어)는 반쪽짜리가 된다.
   - velocity->amp가 선형 `velocity_` 곱이다. SF2는 dB 스케일이 표준이고,
     대부분의 뱅크가 그에 맞춰 보정되어 있다.
2. **보이스 drive가 base-rate 무대조정 비선형.** `curves::{Tanh,Tube,Transformer}::f`
   를 샘플레이트 그대로 적용한다(Sampler.cpp:397-402). ADAA 커널은
   `x10_dsp`에 이미 존재하고 정확도 테스트도 있다 - 보이스 경로에만 안 쓰였다.
   고drive에서 이미징/aliasing이 생기고, bus oversampling은 그 뒤 단계라
   이미 발생한 foldback을 못 지운다.
3. **ADSR이 전 구간 선형.** 음색마다 다른 attack/release 곡률이 표준이고,
   특히 짧은 release는 선형이면 페이드 테일이 딱딱하다.
4. **핑퐁 딜레이가 정수 샘플 지연.** BPM 변경 시 지연 시간이 샘플 단위로
   점프해 클릭이 날 수 있고, subdiv는 1/8D 고정, feedback 댐핑(딜레이
   반복의 고역 감쇠)이 없다.
5. **리버브가 JUCE 레거시 Freeverb.** 동작은 하지만 방감도·밀도가 단순하다.
6. **게인 스테이징: 일반 3음 화음도 safety knee 근접**(headroom probe 측정).
   ceiling에 기대는 구조라 FX가 붙으면 더 자주 닿는다.
7. **스테레오 width가 내부 상수 0.60.** P1 정규화로 연속성은 해결됐으므로
   이제 사용자 파라미터로 노출할 수 있다.
8. **P3 잔여 CPU miss 84회**(96k·128 voices). 하드 리셋은 아니지만 96k
   세션에서의 마진 문제로 남는다.

### UI/UX 격차

1. **인터랙션 기본기 부재**: 더블클릭 기본값 리셋, 미세조정(Shift/Alt),
   파라미터 툴팁이 없다. Knob readout은 드래그 중에만 보인다.
2. **피드백 부족**: 키보드 컴포넌트가 있지만 연주 중인 노트 하이라이트가
   없고, peak meter에 클립 래치가 없다. MIDI CC로 움직이는 노브 시각 피드백은
   20 Hz polling에 의존한다.
3. **프리셋 플로우**: 오버레이는 있으나 헤더에 prev/next가 없고,
   검색/뱅크 필터가 없다. 뱅크가 커지면 탐색 비용이 커진다.
4. **HiDPI**: 스킨이 래스터 PNG 하나라 2345px 최대 확대 구간에서
   아트워크가 뿌옇다. 고DPI 모니터에서 콘트롤 오버레이만 선명하다.
5. **접근성**: 컨트롤은 accessible tree에 있지만 명시적
   `setAccessible`/`setDescription`, 키보드 포커스 순서, 값 읽기 정리가
   검증되지 않았다.

## 실행 단계

순서는 체감 효과/리스크 기준이다. 각 단계는 독립 머지 가능 크기로 쪼갠다.

| 단계 | 작업 | 완료 기준 |
|---|---|---|
| SQ-1 | **SF2 fidelity 1차**: `Sample`(또는 별도 zone 레코드)에 volumeEnvelope, attenuationDb, pan, exclusiveClass 복사. 보이스가 존 env를 쓰되 전역 ADSR은 스케일 오프셋으로 유지. 멀티 레이어: match 버퍼 확장(예 8) 후 존별 보이스 할당. exclusiveClass로 같은 그룹 보이스 초크. | 동일 뱅크를 FluidSynth/sforzando 렌더와 구조 비교(노트 오프 후 초크, 레이어 수, 상대 음량). 하이햇 오픈->클로즈 초크 재현. RT 무할당 유지. |
| SQ-2 | **SF2 fidelity 2차**: modEnv->filter/pitch, vibLFO/modLFO 존 파라미터, velocity->amp를 dB 스케일로. velocity 곡선은 뱅크 의도가 기준이고, 표현력 조정용 UI 곡선은 별도 파라미터로 분리한다. | SF2 스펙 테스트 벡터(timecents/dB 변환은 이미 flattener 테스트가 커버). velocity 1/64/127의 상대 레벨이 레퍼런스 렌더와 일치. |
| SQ-3 | **보이스 drive ADAA**: `x10::dsp::Adaa1`를 per-voice tanh/tube/transformer에 적용. Tanh는 ADAA1로 가능, Tube/Transformer는 비용이 크면 2x 내부 OS로 대체 검토. | 기존 `test_adaa_correctness` 방식으로 보이스 경로에도 스펙트럴 fixture 추가. drive 100%에서 in-band aliasing 감소 측정. CPU 증가율 P3 매트릭스로 확인. |
| SQ-4 | **엔벨로프 곡률**: attack/release에 곡선 옵션(또는 SF2 표준 concave attack). 전역 ADSR 파라미터 기본값에서 기존 소리 유지 여부 결정 - 바뀌면 레벨 매칭 A/B 후 사용자 승인. | 단음 attack/release 테일의 클릭/펌핑 스펙트럴 검사 + 청취 A/B. |
| SQ-5 | **딜레이 개선**: 분수 지연(allpass 또는 sinc 인터폴)으로 BPM 추적 시 클릭 제거, feedback 루프에 1차 LPF 댐핑, 지연 시간 스무딩. subdivision 선택은 새 파라미터가 필요하므로 추가 시 기본값을 기존 1/8D와 동일하게. | BPM 램프 중 잡음 없음, 피드백 테일 고역 감쇠 측정. |
| SQ-6 | **리버브 품질** (선택): Freeverb 유지하되 damping/diffusion 개선하거나, 소형 FDN/Dattorro로 교체. eon_dsp 벤더드 코드 재사용 가능성 검토. 교체는 기본값에서 소리가 바뀌므로 레벨 매칭 A/B 승인 필요. | impulse tail RT60/스펙트럼 기록 + 청취 A/B. |
| SQ-7 | **게인 스테이징**: headroom probe가 보여준 ceiling 근접 해소 - 내부 믹스 레벨 또는 기본 out.trim 조정. 기존 세션 소리가 바뀌므로 측정->A/B->승인 순서. | 동일 재생 조건에서 ceiling 도달률 감소, 청취 레벨 매칭. |
| SQ-8 | **P3 잔여 miss**: 96k/128 voices의 84회 miss 원인 분리(보이스 루프 vs 인터폴 vs 필터). 선택 사항: voice 렌더 SIMD, interpolator 계수 캐시 힌트. | 동일 12조건 매트릭스에서 0 miss. |
| SQ-9 | **Width 파라미터 노출**: 신규 파라미터 추가(기존 ID 불변, 추가는 세션 호환). 기본값 60%로 현재 소리 유지. | width 0->100 스윕 시 파워 단조·연속, 자동화 스무딩. |

| 단계 | 작업 | 완료 기준 |
|---|---|---|
| UX-1 | **인터랙션 기본기**: 노브 더블클릭->기본값, Shift 미세조정(0.1x), Alt 순환 세그먼트, 드래그 readout에 단위 표시, 휠 스텝을 파라미터 단위에 맞춤. | 각 노브에서 리셋/미세/휠 동작 수동 확인 + ui_shot 회귀. |
| UX-2 | **연주 피드백**: 키보드 활성 노트 하이라이트(processor의 노트 상태를 20 Hz timer로 폴링), peak meter 클립 래치+클릭 리셋, GR meter 피크 홀드. | 노트 on/off가 키베드에 반영, 클립 발생 시 래치 점등. |
| UX-3 | **프리셋 탐색**: 헤더에 prev/next 프리셋 순회, 오버레이에 이름 검색·뱅크 필터, 최근 로드 표시. | 프리셋 목록 순회와 검색이 dirty 플로우와 충돌 없이 동작. |
| UX-4 | **HiDPI·접근성**: 스킨 @2x 자산 또는 핵심 요소 vector paint 폴백, 컨트롤별 accessible 설명, 키보드 포커스 순서 정리. | 200% 스케일 스크린샷에서 아트워크 선명도, 스크린리더 라벨 검토. |
| UX-5 | **호스트 UX 확인**: Ableton/REAPER에서 리사이즈 핸들, 자동화 제스처(우클릭 등), 상태 저장/복원 왕복. | 두 호스트에서 저장->재오픈->동일 상태. |

## 제약과 의존

- 파라미터 ID·PLUGIN_CODE 불변. 신규 파라미터는 추가만 허용하고 기본값은
  기존 소리를 재현해야 한다. 기존 소리가 바뀌는 단계(SQ-4 곡률 기본값, SQ-6
  리버브 교체, SQ-7 게인)는 레벨 매칭 A/B 후 사용자 승인이 필요하다.
- SQ-1/SQ-2는 Region 스키마가 이미 필드를 갖고 있어 flattener 변경 없이
  가능하다. Sample 구조 확장과 getSample의 다중 매치가 핵심 설계 지점이다.
- 오디오 스레드 실시간 규칙(무할당·무록·무 I/O)은 모든 DSP 단계에 적용.
  존 데이터는 로딩 시 확정하고 보이스는 const 참조만 쥔다(현재 패턴과 동일).
- 검증 게이트는 별개다: 빌드 -> dev/plugin CTest -> pluginval S10 -> auval ->
  eonqc 매니페스트 -> REAPER/Ableton 로드 -> 레벨 매칭 청취. 모든 증거에
  바이너리 SHA256, 호스트 버전, SR, 버퍼를 기록한다.
- 현재 워크트리에 하드코딩 수정 3건 등 미커밋 변경이 있다. 각 단계 시작 전
  git status 기준점을 다시 잡는다.

## 권장 첫 삼인분

1. **SQ-1** - 체감 차이가 가장 크고, 이미 계산된 데이터를 연결만 하는 일.
2. **UX-1+UX-2** - DSP와 독립이라 SQ-1 리뷰와 병행 가능, 리스크 낮음.
3. **SQ-3 ADAA** - 라이브러리가 이미 있어 diff가 작고, 측정 가능한 개선.


## 진행 기록

### SQ-1 — SF2 존 데이터 연결 (2026-09-29, 미커밋 워크트리)

구현

- `Sample`에 `volumeEnvelope`/`attenuationDb`/`pan`/`exclusiveClass`를 추가하고
  `SF2Loader::loadFile`이 Region의 네 필드를 복사한다.
- `SF2Loader::getSamples(bank, program, key, velocity, span)` 신규. `RegionIndex::match`를
  8칸 버퍼로 호출해 겹치는 존을 최대 8개 돌려주고, 버퍼보다 많이 매치되면 개수를 그대로
  반환해 잘림을 호출자가 알 수 있게 한다. `getSample`은 이 함수의 1칸 호출로 위임한다.
- `VoicePool::start(std::span<const Sample* const>)` 추가(`maxLayersPerNote = 8`). 같은
  노트의 기존 레이어는 제자리 재사용, 레가토는 리드 노트의 전 레이어를 리타겟, 남는
  레이어는 retire. `noteToVoice_` 배열을 걷어내고 노트→보이스 조회를 polyphony 범위
  선형 스캔(≤128, 무할당)으로 바꿨다. `setBankTokenForNote`가 한 노트의 모든 레이어에
  같은 BankToken을 준다.
- `Voice::VolumeEnvelope`(delay/attack/hold/decay/sustain) 추가. 보이스 레벨은
  전역 ADSR × 존 envelope이고, `attenuationDb`는 `10^(-clamp(db,0,144)/20)` 게인이다.
- exclusiveClass 초크는 하드 컷이 아니라 존 release를 0.5~20 ms로 클램프한 페이드로
  끝낸다. 초크는 보통 상대 노트의 최대점에서 일어나므로 즉시 자르면 클릭이 난다.
- 존 pan은 스테레오 이미지의 우선권을 갖고, 남는 폭에만 기존 합성 스프레드를 적용한다.
  pan 0에서 등파워 법칙은 이전과 동일하다.

설계 결정

- SF2 존 release는 대체물이 아니라 **하한값**이다. 유효 release = max(UI release,
  존 release)를 전역 ADSR에 넘겨 지수 감쇠 곡선을 그대로 유지한다. 존 release를 별도
  페이드로 곱하면 기본 1 ms 존 release가 UI release(기본 50 ms)를 무력화한다
  (구현 중 테스트로 확인). 존 sustain이 0이면 디케이가 끝날 때 보이스를 retire해
  무음 보이스가 슬롯을 붙잡지 않게 한다.

증거

- 빌드: `cmake --preset plugin` 통과, VST3/AU/Standalone 재생성. SHA256
  VST3 `0bf816a7fc038485ae5380aa2372764505762a3abc84cb3dfc8c0da8872785b7`,
  AU `4afc8405c22c43619b1981fcee1b5855e7699f8f3e62e934046996eb4b383c4f`,
  rompler_tests `28bd053c31adf53114a6282c795994412ba6392548a2ff06759683b7b3f1ca82`.
- `ctest --preset plugin`: 176/176 통과(`pluginval_vst3_strictness10` 포함).
  `ctest --preset dev`: 88/88 통과. SR 48 kHz 기준.
- 신규 테스트: 존 겹침 레이어의 시작/릴리스, exclusiveClass 초크 페이드,
  attenuation 6.0206 dB = 0.5배, 존 pan -1 → 좌 채널 전량, 실제 뱅크
  (`Voice_Erhu.sf2`)에서 2존·스테레오 페어·pan/attenuation 범위 확인.
- RT 무할당: 기존 스테레오 렌더 무할당 테스트 통과. 신규 경로는 `std::array`/`std::span`
  과 컨테이너 조회만 쓴다.

미검증(완료 기준 잔여)

- FluidSynth/sforzando 렌더와의 구조 비교(초크 시점, 레이어 수, 상대 음량)는 미실행이다.
- auval, eonqc 매니페스트, REAPER/Ableton 로드, 레벨 매칭 청취는 미실행이다.
- 초크 페이드 상한(20 ms)과 "존 release = 하한" 정책은 청취 승인 대상이다.

별건

- `cmake --preset plugin`이 melatonin_inspector 모듈 헤더를 찾지 못해 실패하던 문제는
  FetchContent `SOURCE_DIR`을 `.../melatonin_inspector`로 고정해 해결했다(디렉터리 이름이
  모듈 이름을 결정하는 JUCE 규칙 때문). SQ-1과 무관하지만 plugin 프리셋 빌드의 전제다.

### UX-1 / UX-2 — 인터랙션 기본기와 연주 피드백 (2026-09-29, 미커밋 워크트리)

이미 들어와 있던 부분은 그대로 두고, 비어 있던 항목과 실제로 동작하지 않던 항목을 채웠다.

구현/수정

- **Alt 역순환**: `Switch`가 클릭/드래그로 다음 세그먼트로만 갔고, 클릭 후 릴리스에서 한 번 더
  스텝이 올라가는 문제가 있었다. 이제 `mouseUp`은 클릭 제스처일 때만 스텝하고,
  Alt-클릭/우클릭은 이전 세그먼트로 간다(휠도 같은 규칙).
- **휠 스텝**: 노브 휠이 파라미터 `interval`을 그대로 스텝으로 써서, 0..100 범위에
  interval 0.01인 파라미터는 노치당 0.01만 움직여 사실상 무효였다. JUCE `Slider`와 같은
  규약(노치당 범위의 0.15, 단 `interval`을 하한으로)으로 바꿨다. `Stepper`는 눈금형
  컨트롤이므로 휠 1노치 = `interval` 1스텝으로 고정했고(스무스 휠의 0.1 delta가 정수
  파라미터에서 반올림으로 사라지던 문제), Shift 미세조정도 추가했다.
- **readout 단위**: `AudioParameterFloat::getText`는 숫자만 돌려주고 단위는 호스트가
  `getLabel()`로 붙인다. 그래서 화면 readout에 단위가 없었다. Knob이 슬라이더 텍스트에
  단위를 붙이도록 했고, 값을 노출하는 `readoutText()`를 공개 API로 추가했다.
- **GR 피크 홀드**: `GainReductionMeter`에 홀드 마커를 추가했다. 새 최대값으로 즉시
  올라가고 UI 업데이트마다 0.9 dB씩 내려가며, 클릭하면 현재 감쇠량으로 내려간다.
  스킨/일반 페인트 양쪽에 마커를 그린다.
- 앞선 작업으로 이미 있던 더블클릭 기본값 리셋, Shift 미세조정, 클립 래치,
  키베드 활성 노트 표시는 유지하고 검증만 추가했다.

증거

- 신규 `test_editor_interaction.cpp`(8 케이스, `[ui][interaction]`): 더블클릭 리셋,
  40 px 드래그와 휠 노치의 스케일과 Shift 1/10, readout 단위, Switch 클릭/Alt/드래그
  스텝 수, Stepper 휠 1노치=1스텝, 클립 래치와 클릭 리셋, GR 홀드 상승/감쇠/클릭 리셋,
  Keyboard `isNoteLit`.
- `test_sf2_playback.cpp`에 프로세서 통합 케이스 추가: 노트온 후 활성 노트 마스크에
  비트가 서고, 노트오프 후 릴리스 테일 동안 유지되다 사라진다.
- `rompler_tests` 전체 96 케이스 통과. `ctest --preset plugin` 185/185,
  `ctest --preset dev` 88/88 통과. VST3 SHA256
  `f177bdb55961a0d7773af2af0c8dc6b4dcdab82fc63541c48659fb8d93143227`,
  AU `deea381304dd426c39fb482092475436bcb982630fd6882c05ce32fa5e4f47d0`,
  rompler_tests `ec68cbf7682274ba9dc6d1af22726037c6e337f55feb5681cbf49385110aec2d`.
- `ui_shot` 렌더(`/tmp/aoi-ui-ux2.png`, 1563x1006)로 레이아웃 회귀 없음 확인.

미검증

- 노브 리셋/미세조정/휠, Alt 순환, 클립 래치, GR 홀드는 단위 테스트로만 확인했다.
  실제 마우스/트랙패드 감도와 청감 확인은 사용자 몫이다.
- HiDPI(UX-4), 프리셋 탐색(UX-3), 호스트 UX(UX-5)는 아직 손대지 않았다.

### SQ-3 착수 조사 (2026-09-29)

먼저 현재 보이스 drive의 전달 특성을 측정했다. 숨김 진단
`rompler_tests '[diagnostic]'`(997 Hz 풀스케일 단음, 1 voice, 각 커브) 결과:

| DRIVE(%) | curve 0 peak | curve 1 peak | curve 2 peak |
| ---: | ---: | ---: | ---: |
| 0 | 1.000 | 1.000 | 1.000 |
| 1 | 0.720 | 0.797 | 0.471 |
| 6 | 0.483 | 0.577 | 0.334 |
| 12 | 0.251 | 0.313 | 0.201 |
| 25 | 0.056 | 0.070 | 0.053 |
| 50 | 0.00316 | 0.00395 | 0.00315 |
| 100 | 1.00e-05 | 1.25e-05 | 1.00e-05 |

즉 출력 피크가 정확히 `-DRIVE` dB다(12 → -12 dB, 50 → -50 dB, 100 → -100 dB).
wet 경로가 `f(gain*x)/gain`이고 `gain = 10^(DRIVE/20)`이므로, 포화 커브의 출력을
커브 **이전** 게인으로 나눈 것이 원인이다. DRIVE는 새터레이터가 아니라 페이더로
동작하며, 1% 이상에서는 blend가 이미 1이라 노브 대부분 구간이 사실상 음량 감소다.

같은 경로에서 SQ-3의 ADAA를 넣으면 별개 문제가 하나 더 드러난다. 2026-09-08
`aoi-yume-quality-stage1`에서 이미 ADAA를 시도하고 **의도적으로 채택하지 않았다**:
저레벨 20 kHz 신호가 ADAA에서 11.740 dB 감쇠했고, residual ADAA는 구동 fundamental을
+1.30 dB 바꿨다. 당시 벤치(`tools/drive_quality_bench.cpp`)는 지금 저장소에 없다.
이번 계획서는 "ADAA 커널이 이미 있고 보이스 경로에만 안 쓰였다"고 적었지만, 실제로는
이 경로에서 한 번 검토 후 보류된 이력이 있다. 그래서 SQ-3는 (1) 보이스 경로에 ADAA를
넣고 스펙트럴 fixture로 이득을 측정하고, (2) 같은 fixture로 소신호 고역 손실을 정량화해
채택 여부를 판단할 수 있는 증거를 만드는 것으로 진행한다.

두 변경 모두 DRIVE > 0인 패치의 소리를 바꾸므로(SQ-7 게인과 같은 성격), 기본값
DRIVE 0은 그대로 두고 사용자 승인용 측정치로 올린다.

### SQ-3 결과 (2026-09-30)

구현은 `Voice::Drive` 스테이지로 분리했다. 한 타입이 ADAA1 경로와 기존 직접 평가 경로를
모두 갖고, 기본값은 직접 평가(= 기존 소리)다. `setAntialiasing(true)`로 전환하며 전환 지점은
프로세서→보이스 배선이 아직 없다. 소리를 바꾸는 변경이라 파라미터로 노출하지 않았다.

측정 조건은 보이스 동작점이다. 풀스케일 샘플, DRIVE 12%, 커브 입력 진폭 4.0(라이브러리
gate의 동작점과 같다). 게이트 비교는 양쪽이 **같은 커브 입력 진폭**을 보도록 레퍼런스를
스케일해야 공정하다. 처음에 스톡 게이트를 그대로 쓰자 후보만 4배 더 구동되어 −34.8 dB가
나왔다. 공정하게 다시 재면:

| 커브 | 직접 대비 NMR 개선 |
| --- | ---: |
| Tanh | +6.84 dB |
| Tube | +7.15 dB |
| Transformer | +7.67 dB |

라이브러리 자체 gate가 문서화한 6.8-8.6 dB 밴드와 일치한다. 안티앨리어싱 자체는 동작한다.

비용은 두 가지다.

- **소신호 고역**: 20 kHz(진폭 0.02)에서 직접 대비 11.73 dB 감쇠. 2026-09-08 stage1 문서의
  11.740 dB와 같은 값이 재현됐다. 1차 ADAA는 소신호 극한에서 곡선의 2점 평균이 되므로
  전달함수가 cos(pi f / fs)이다. 8 kHz에서 약 −0.9 dB, 12 kHz에서 −3.5 dB, 20 kHz에서 −11.7 dB.
  즉 DRIVE를 1%만 올려도 blend가 1이라 상단이 눌린다.
- **스테이지 CPU**: 무작위 신호, gain 4.0에서 직접 4.9 ns/sample, ADAA 17.2 ns/sample(3.5배).
  128 voices/48 kHz면 +12.3 ns x 128 x 48000 = 초당 0.076 s, 한 코어의 7.6%이고
  512-sample 블록당 약 806 us다. 96k/512/128 baseline 5580 us 대비 +14%다.

판단: 이득 +7 dB에 비해 고역 11.7 dB와 +14% CPU가 크고, 둘 다 소리를 바꾸는 변경이라
기본값을 직접 평가로 두었다. 계획서의 "ADAA 커널이 이미 있고 보이스 경로에만 안 쓰였다"는
전제는 이 경로에서 ADAA를 한 번 시도해 보류한 이력(2026-09-08 stage1)을 반영하지 못한 것이다.

다음 순서를 권한다. (1) DRIVE 게인 보상 결정 — 현재 DRIVE는 새터레이터가 아니라 페이더이고,
그 붕괴가 고drive의 aliasing을 −12 dB 이하로 눌러 가려 놓았다. (2) 그 다음 안티앨리어싱 방식
결정 — 이득/비용만 보면 2x 내부 오버샘플링(하프밴드 + 지연 보상)이 ADAA1보다 낫다.
지연·CPU·레벨 설계와 청취가 필요하므로 별도 단계다.

테스트 `test_voice_drive.cpp`:

- 기본 경로가 커브별로 기존 직접 커브와 1e-6 이내로 같다 — 기본 소리 불변을 고정한다.
- ADAA 경로의 NMR 개선 >= 5 dB(실측 6.8/7.1/7.7).
- 보이스 렌더의 NMR이 같은 곡선을 보이스의 dry 출력에 직접 적용한 값과 2 dB 이내다.
  (레퍼런스를 원본 샘플로 구동하면 보이스 경로가 드라이브 앞에서 0.70으로 스케일하므로
   레퍼런스만 더 세게 구동되어 −6.5 dB 차이가 났다. 그래서 dry 렌더를 레퍼런스 입력으로 쓴다.)
- 20 kHz 소신호 손실 10-13 dB 구간을 고정한다(실측 11.73).
- 숨김 `[.][benchmark][adaa]`가 두 경로의 sample당 비용과 비율을 출력한다.
