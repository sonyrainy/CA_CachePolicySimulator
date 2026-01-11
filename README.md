## Goal
**제한된 하드웨어 자원(Cache Size 1024B, Block Size 8B, 8-way Associativity)** 에서 LRU/FIFO 대비 더 낮은 miss rate를 목표로 새로운 교체 정책(NEW)을 설계/구현해보자!


<br>


## Motivation
LRU는 스트리밍/스캔(1회성 접근) 패턴에서 캐시를 쉽게 오염시킬 수 있다. 즉, 기존에 반복적으로 사용되던 hot line을 밀어내는 문제가 있을 수 있다.
이 문제를 줄이기 위해, 단순 recency 대신 **frequency + aging** 기반으로 “자주 쓰이는 값은 오래 유지, 오래 안 쓰이면 자연스럽게 퇴출”되도록 설계하여 구현했다.


<br>


## Policy: NEW (2-bit Frequency + aging)
각 cache line에 2-bit `priority_counter`(0~3)를 둔다.
- 의미: 0 = 교체 후보, 3 = 보호 대상


<br>


## Mechanism
- **Hit**
  - `priority_counter = min(3, priority_counter + 1)`
  - 자주 조회될수록 점수가 높아져 교체 대상에서 멀어진다.

- **Insert**
  - 새로 들어오는 line은 `priority_counter = 1`
  - 0(바로 교체)보다는 높지만, 2 이상(검증된 값)보다는 낮은 정도의 우선순위를 부여한다. 

- **Miss & Eviction**: priority_counter가 0인 라인을 찾아 교체시킨다.
  - `priority_counter == 0` 인 line을 찾아 교체한다.
  - 없으면 모든 line의 `priority_counter`를 1씩 감소(aging)시키고, 0이 생길 때까지 반복한다.
  - 결과적으로 “시간이 지나도 참조되지 않는 값”은 점수가 깎여 퇴출된다.


<br>


## Complexity / Overhead
- Hit 경로는 O(1)
- Miss 경로는 최악의 경우 set 내 모든 line을 여러 번 스캔할 수 있어 O(ways * aging_steps)라고 볼 수 있다.

→ ("Goal"에서 제한한 조건인) 8-way 제한에서는 교체 후보 탐색이 최대 8라인 스캔으로 수렴해 오버헤드가 제한적이다. 따라서 구현 복잡도를 낮추는 방향으로 구현했다.


<br>


## +) 8-way 제한이 없을 때 어떻게 시뮬레이터를 개선시킬 수 있을까?
- 캐시 안의 값들 점수를 매번 전부 1씩 내리는 대신, 시간이 한 번 지났다는 표시(숫자)를 전역적으로 두어 1씩 올리는 식으로 구현한다.
- 그리고 어떤 값을 볼 때(Hit, Insert, Miss)만 그 동안 지난 횟수만큼 점수를 계산해서 빼면(aging 연산을 필요한 순간에만 하는 것이다.), 전부를 매번 고치는 것보다 효율적일 것이다.
