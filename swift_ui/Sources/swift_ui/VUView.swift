import SwiftUI

struct VUView: View {
    var body: some View {
        VStack {
            Text("VU Library")
                .font(.largeTitle)
                .fontWeight(.bold)
                .foregroundColor(Theme.textPrimary)
            
            Text("Coming soon...")
                .foregroundColor(Theme.textSecondary)
                .padding()
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Theme.background)
    }
}
