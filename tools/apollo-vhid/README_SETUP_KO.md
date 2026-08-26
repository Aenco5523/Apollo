# Apollo V-HID 테스트 도구

이 폴더의 도구는 `Root\ApolloVhid` 장치와 Apollo Portable 테스트를 다룹니다.

## 파일

- `ApolloVhidDevnode.exe`: `Root\ApolloVhid` root devnode 생성/상태 확인/제거용 최소 SetupAPI helper
- `Install-ApolloVhid.ps1`: 카탈로그 서명이 Windows에서 `Valid`인지 확인한 뒤 devnode 생성 + `pnputil /add-driver ... /install`
- `Remove-ApolloVhidDevice.ps1`: V-HID root 장치만 제거. 드라이버 저장소의 패키지는 일부러 남김
- `Start-ApolloVhidPortable.ps1`: Portable ZIP을 `Runtime`에 풀고 전용 `Config\sunshine.conf`에 `virtual_hid = true`를 적용해 foreground로 실행

## 보안 원칙

설치 스크립트는 드라이버 카탈로그 서명이 유효하지 않으면 즉시 중단합니다. 드라이버 서명 검증, Secure Boot, 테스트 서명 정책 등 Windows 보안 설정을 끄거나 우회하지 않습니다.

현재 CI의 `SignMode=Off` 개발 드라이버는 이 설치 스크립트가 정상적으로 거부하는 것이 의도된 동작입니다. 실제 설치 테스트에는 적절하게 서명된 드라이버 패키지가 필요합니다.

## Portable 테스트

드라이버가 정상 설치된 뒤 PowerShell에서 다음 스크립트를 실행합니다.

```powershell
.\Tools\Start-ApolloVhidPortable.ps1
```

전용 설정 파일을 사용하므로 기존 Apollo 설치의 `sunshine.conf`를 수정하지 않습니다. Apollo 로그에서 다음 메시지가 보이면 V-HID backend가 열린 것입니다.

```text
Virtual HID keyboard/mouse backend enabled
```

장치를 열 수 없으면 기존 SendInput 경로로 폴백합니다.
