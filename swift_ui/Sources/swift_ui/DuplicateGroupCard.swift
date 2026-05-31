import SwiftUI

struct DuplicateGroupCard: View {
    let group: Vurio_Dedup_DuplicateGroup
    var duplicateTightness: Double
    var isEjecting: Bool
    var isSelected: Bool
    @Binding var selectedCandidateIndex: Int
    var onThumbnailTapped: (Int) -> Void
    var onEjectSet: ([String]) -> Void
    var onRestoreSet: ([String]) -> Void
    
    @AppStorage("advancedOverlayEnabled") private var advancedOverlayEnabled = false
    @State private var isRepEjectHovered = false
    
    var representative: Vurio_Dedup_DuplicateFile? {
        group.files.first(where: { $0.isRepresentative }) ?? group.files.first
    }
    
    var candidates: [Vurio_Dedup_DuplicateFile] {
        if group.files.contains(where: { $0.isRepresentative }) {
            return group.files.filter { !$0.isRepresentative }
        } else {
            return Array(group.files.dropFirst())
        }
    }
    
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .top, spacing: 20) {
                if let rep = representative {
                    DuplicateMemberView(file: rep, isRep: true, overlayText: "ID: \(group.groupID)", isEjecting: isEjecting, onRestore: {
                        let allPaths = group.files.map { $0.filePath }
                        onRestoreSet(allPaths)
                    }, onEject: {
                        let allPaths = group.files.map { $0.filePath }
                        onEjectSet(allPaths)
                    }, onEjectHoverChanged: { hovering in
                        withAnimation(.easeInOut(duration: 0.2)) {
                            isRepEjectHovered = hovering
                        }
                    }, onThumbnailTapped: {
                        if !isEjecting {
                            if let idx = group.files.firstIndex(where: { $0.filePath == rep.filePath }) {
                                onThumbnailTapped(idx)
                            }
                        }
                    })
                        .padding(.top, 25)
                        .padding(.bottom, 25)
                }
                
                if !candidates.isEmpty {
                    Divider()
                        .frame(height: 70)
                        .background(Theme.borderActive)
                        .padding(.top, 30)
                        .padding(.bottom, 30)
                }
                
                VerticalScrollingFixView {
                    ScrollViewReader { proxy in
                        ScrollView(.horizontal, showsIndicators: false) {
                            LazyHStack(spacing: 12) {
                                ForEach(Array(candidates.enumerated()), id: \.offset) { index, file in
                                    DuplicateMemberView(file: file, isRep: false, isEjecting: isEjecting, onRestore: {
                                        onRestoreSet([file.filePath])
                                    }, onEject: {
                                        onEjectSet([file.filePath])
                                    }, onThumbnailTapped: {
                                        if !isEjecting {
                                            if let idx = group.files.firstIndex(where: { $0.filePath == file.filePath }) {
                                                onThumbnailTapped(idx)
                                            }
                                        }
                                    }, isFocusedCandidate: isSelected && selectedCandidateIndex == index)
                                    .id(index)
                                }
                            }
                            .padding(.top, 30)
                            .padding(.bottom, 30)
                            .padding(.leading, 6)
                            .padding(.trailing, 12)
                            .overlay(alignment: .bottomLeading) {
                                let indices = candidates.enumerated().compactMap { index, file in
                                    let score = file.similarityScore > 0 ? file.similarityScore : 1.0
                                    return (1.0 - score) <= duplicateTightness ? index : nil
                                }
                                
                                if let first = indices.first, let last = indices.last {
                                    let count = last - first + 1
                                    let leftOffset = CGFloat(first) * (70 + 12) + 6 - 4 // Account for leading padding and balance the +8 width
                                    let width = CGFloat(count) * 70 + CGFloat(count - 1) * 12 + 8
                                    
                                    let toRestore = indices.compactMap { candidates[$0] }.filter { $0.isEjected }.map { $0.filePath }
                                    let toEject = indices.compactMap { candidates[$0] }.filter { !$0.isEjected }.map { $0.filePath }
                                    let isRestoreMode = toEject.isEmpty
                                    
                                    BracketView(
                                        width: width,
                                        isRestoreMode: isRestoreMode,
                                        isEjecting: isEjecting,
                                        action: {
                                            if isRestoreMode {
                                                onRestoreSet(toRestore)
                                            } else if !toEject.isEmpty {
                                                onEjectSet(toEject)
                                            }
                                        }
                                    )
                                    .padding(.leading, leftOffset)
                                    .padding(.bottom, 6)
                                    .animation(.spring(response: 0.3, dampingFraction: 0.7), value: width)
                                    .animation(.spring(response: 0.3, dampingFraction: 0.7), value: leftOffset)
                                }
                            }
                        }
                        .onChange(of: selectedCandidateIndex) { old, newIdx in
                            if isSelected {
                                let safeIdx = min(max(0, newIdx), max(0, candidates.count - 1))
                                if safeIdx != newIdx {
                                    DispatchQueue.main.async {
                                        selectedCandidateIndex = safeIdx
                                    }
                                } else {
                                    withAnimation(.easeOut(duration: 0.15)) {
                                        proxy.scrollTo(safeIdx, anchor: .center)
                                    }
                                }
                            }
                        }
                        .onChange(of: isSelected) { old, newlySelected in
                            if newlySelected {
                                let safeIdx = min(max(0, selectedCandidateIndex), max(0, candidates.count - 1))
                                DispatchQueue.main.async {
                                    withAnimation(.easeOut(duration: 0.15)) {
                                        proxy.scrollTo(safeIdx, anchor: .center)
                                    }
                                }
                            }
                        }
                    }
                }
                .frame(height: 130)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(
            RoundedRectangle(cornerRadius: 12)
                .fill(isSelected ? Theme.surfaceElevated.opacity(0.8) : Theme.surface.opacity(0.5))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 12)
                .stroke(isRepEjectHovered ? Theme.error : (isSelected ? Theme.textAccent : Theme.border), lineWidth: (isRepEjectHovered || isSelected) ? 2 : 1)
        )
        .padding(.bottom, 4)
    }
}

struct DuplicateMemberView: View {
    let file: Vurio_Dedup_DuplicateFile
    let isRep: Bool
    var overlayText: String? = nil
    var isEjecting: Bool = false
    var onRestore: (() -> Void)? = nil
    var onEject: (() -> Void)? = nil
    var onEjectHoverChanged: ((Bool) -> Void)? = nil
    var onThumbnailTapped: (() -> Void)? = nil
    var isFocusedCandidate: Bool = false
    
    @State private var isHovered = false
    @State private var isButtonHovered = false
    @AppStorage("advancedOverlayEnabled") private var advancedOverlayEnabled = false
    
    var body: some View {
        let size: CGFloat = isRep ? 80 : 70
        let cornerRadius: CGFloat = 8
        
        ZStack {
            ThumbnailView(filePath: file.filePath, size: CGSize(width: size, height: size))
                .frame(width: size, height: size)
                .cornerRadius(cornerRadius)
                .opacity(file.isEjected ? 0.3 : 1.0)
                .contentShape(Rectangle())
                .onTapGesture {
                    onThumbnailTapped?()
                }
            
            if file.isEjected {
                Button(action: {
                    onRestore?()
                }) {
                    CustomRestoreShape()
                        .stroke(style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                        .frame(width: 16, height: 16)
                        .frame(width: 40, height: 40)
                        .foregroundColor(Theme.textPrimary)
                        .background(
                            Circle()
                                .fill(isButtonHovered && !isEjecting ? Theme.success : Theme.success.opacity(0.8))
                                .shadow(color: .black.opacity(0.6), radius: 4, x: 0, y: 3)
                        )
                        .overlay(Circle().stroke(Color.black.opacity(0.6), lineWidth: 0.5))
                }
                .buttonStyle(.plain)
                .disabled(isEjecting)
                .opacity(isEjecting ? 0.5 : 1.0)
                .onHover { hovering in
                    withAnimation(.easeInOut(duration: 0.1)) {
                        isButtonHovered = hovering
                    }
                }
            }
        }
        .overlay(
            RoundedRectangle(cornerRadius: cornerRadius)
                .stroke(file.isEjected ? Theme.error : (isFocusedCandidate ? Theme.textAccent : (isRep ? Theme.textAccent : (isHovered ? Theme.textPrimary : Theme.border))), lineWidth: isFocusedCandidate ? 3 : (isRep ? 2 : (file.isEjected ? 2 : 1)))
        )
        .overlay(alignment: .topLeading) {
            if advancedOverlayEnabled {
                if let text = overlayText {
                    Text(text)
                        .font(.custom("Menlo", size: 9))
                        .foregroundColor(Theme.success)
                        .offset(y: -14)
                } else if !isRep {
                    Text(String(format: "%.4f", file.similarityScore))
                        .font(.custom("Menlo", size: 9))
                        .foregroundColor(Theme.success)
                        .offset(y: -14)
                }
            }
        }
        .overlay(alignment: .topTrailing) {
            if !file.isEjected && !isEjecting {
                Button(action: {
                    onEject?()
                }) {
                    CustomEjectShape()
                        .stroke(style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                        .frame(width: 10, height: 10)
                        .frame(width: 20, height: 20)
                        .foregroundColor(Theme.textPrimary)
                        .background(
                            Circle()
                                .fill(isButtonHovered ? Theme.error : Theme.error.opacity(0.8))
                                .shadow(color: .black.opacity(0.6), radius: 4, x: 0, y: 3)
                        )
                        .overlay(Circle().stroke(Color.black.opacity(0.6), lineWidth: 0.5))
                }
                .buttonStyle(.plain)
                .offset(x: 8, y: -8)
                .opacity((isHovered || isButtonHovered) ? 1.0 : 0.0)
                .allowsHitTesting(isHovered || isButtonHovered)
                .onHover { hovering in
                    withAnimation(.easeInOut(duration: 0.1)) {
                        isButtonHovered = hovering
                    }
                    onEjectHoverChanged?(hovering)
                }
            }
        }
        .scaleEffect(isHovered && !file.isEjected ? 1.05 : 1.0)
        .background(
            RoundedRectangle(cornerRadius: cornerRadius)
                .fill(Color.black.opacity(0.01))
                .shadow(color: isHovered ? Color.black.opacity(0.3) : Color.clear, radius: 4, x: 0, y: 2)
        )
        .animation(.spring(response: 0.3, dampingFraction: 0.6), value: isHovered)
        .onHover { hovering in
            isHovered = hovering
        }
        .zIndex(isHovered ? 1 : 0)
    }
}

struct BracketShape: Shape {
    var cornerRadius: CGFloat = 8
    func path(in rect: CGRect) -> Path {
        var path = Path()
        let r = min(cornerRadius, rect.height)
        
        path.move(to: CGPoint(x: rect.minX, y: rect.minY))
        path.addLine(to: CGPoint(x: rect.minX, y: rect.maxY - r))
        path.addArc(tangent1End: CGPoint(x: rect.minX, y: rect.maxY), tangent2End: CGPoint(x: rect.minX + r, y: rect.maxY), radius: r)
        path.addLine(to: CGPoint(x: rect.maxX - r, y: rect.maxY))
        path.addArc(tangent1End: CGPoint(x: rect.maxX, y: rect.maxY), tangent2End: CGPoint(x: rect.maxX, y: rect.maxY - r), radius: r)
        path.addLine(to: CGPoint(x: rect.maxX, y: rect.minY))
        
        return path
    }
}

struct BracketView: View {
    var width: CGFloat
    var isRestoreMode: Bool
    var isEjecting: Bool = false
    var action: () -> Void
    @State private var isHovering = false

    var body: some View {
        ZStack(alignment: .top) {
            BracketShape()
                .stroke(isRestoreMode ? Theme.success : Theme.error, style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                .frame(width: width, height: 8)
            
            Button(action: action) {
                if isRestoreMode {
                    CustomRestoreShape()
                        .stroke(style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                        .frame(width: 12, height: 12)
                        .frame(width: 24, height: 24)
                        .foregroundColor(Theme.textPrimary)
                        .background(
                            Circle()
                                .fill(isHovering && !isEjecting ? Theme.success : Theme.success.opacity(0.8))
                                .shadow(color: .black.opacity(0.6), radius: 4, x: 0, y: 3)
                        )
                        .overlay(Circle().stroke(Color.black.opacity(0.6), lineWidth: 0.5))
                } else {
                    CustomEjectShape()
                        .stroke(style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                        .frame(width: 12, height: 12)
                        .frame(width: 24, height: 24)
                        .foregroundColor(Theme.textPrimary)
                        .background(
                            Circle()
                                .fill(isHovering && !isEjecting ? Theme.error : Theme.error.opacity(0.8))
                                .shadow(color: .black.opacity(0.6), radius: 4, x: 0, y: 3)
                        )
                        .overlay(Circle().stroke(Color.black.opacity(0.6), lineWidth: 0.5))
                }
            }
            .buttonStyle(.plain)
            .disabled(isEjecting)
            .opacity(isEjecting ? 0.5 : 1.0)
            .offset(y: -4)
            .scaleEffect(isHovering && !isEjecting ? 1.1 : 1.0)
            .onHover { hovering in
                withAnimation(.easeInOut(duration: 0.1)) {
                    isHovering = hovering
                }
            }
        }
        .frame(width: width, height: 20)
    }
}

struct CustomEjectShape: Shape {
    func path(in rect: CGRect) -> Path {
        let scale = min(rect.width, rect.height) / 24.0
        var path = Path()
        path.move(to: CGPoint(x: 12 * scale, y: 2 * scale))
        path.addLine(to: CGPoint(x: 12 * scale, y: 12 * scale))
        
        path.move(to: CGPoint(x: 12 * scale, y: 2 * scale))
        path.addLine(to: CGPoint(x: 9 * scale, y: 5 * scale))
        path.move(to: CGPoint(x: 12 * scale, y: 2 * scale))
        path.addLine(to: CGPoint(x: 15 * scale, y: 5 * scale))
        
        path.move(to: CGPoint(x: 4 * scale, y: 14 * scale))
        path.addLine(to: CGPoint(x: 4 * scale, y: 18 * scale))
        path.addQuadCurve(to: CGPoint(x: 6 * scale, y: 20 * scale), control: CGPoint(x: 4 * scale, y: 20 * scale))
        path.addLine(to: CGPoint(x: 18 * scale, y: 20 * scale))
        path.addQuadCurve(to: CGPoint(x: 20 * scale, y: 18 * scale), control: CGPoint(x: 20 * scale, y: 20 * scale))
        path.addLine(to: CGPoint(x: 20 * scale, y: 14 * scale))
        return path
    }
}

struct CustomRestoreShape: Shape {
    func path(in rect: CGRect) -> Path {
        let scale = min(rect.width, rect.height) / 24.0
        var path = Path()
        path.move(to: CGPoint(x: 12 * scale, y: 3 * scale))
        path.addLine(to: CGPoint(x: 12 * scale, y: 15 * scale))
        
        path.move(to: CGPoint(x: 8 * scale, y: 11 * scale))
        path.addLine(to: CGPoint(x: 12 * scale, y: 15 * scale))
        path.move(to: CGPoint(x: 16 * scale, y: 11 * scale))
        path.addLine(to: CGPoint(x: 12 * scale, y: 15 * scale))
        
        path.move(to: CGPoint(x: 4 * scale, y: 14 * scale))
        path.addLine(to: CGPoint(x: 4 * scale, y: 18 * scale))
        path.addQuadCurve(to: CGPoint(x: 6 * scale, y: 20 * scale), control: CGPoint(x: 4 * scale, y: 20 * scale))
        path.addLine(to: CGPoint(x: 18 * scale, y: 20 * scale))
        path.addQuadCurve(to: CGPoint(x: 20 * scale, y: 18 * scale), control: CGPoint(x: 20 * scale, y: 20 * scale))
        path.addLine(to: CGPoint(x: 20 * scale, y: 14 * scale))
        return path
    }
}
